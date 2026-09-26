/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radio_stats -- see include/node_mgr/radio_stats.h.
 *
 * v1 record (little-endian, 59 bytes):
 *   0  u8  version = 1
 *   1  u8  flags: bit0 = driver tx-outcome counters present
 *   2  u8  channel
 *   3  u8  role (otDeviceRole: 0 disabled 1 detached 2 child 3 router 4 leader)
 *   4  u16 parent RLOC16 (0xFFFF = none)
 *   6  i8  parent average RSSI dBm (127 = none)
 *   7  u8  parent link quality in
 *   8  u8  parent link quality out
 *   9  u32 uptime s
 *  13  u32 radio tx ok           \
 *  17  u32 radio tx CCA busy      | ESP32 802.15.4 driver, per HAL outcome
 *  21  u32 radio tx no ack        | (patches/zephyr/0001); 0 when flags.bit0 = 0
 *  25  u32 radio tx other        /
 *  29  u32 MAC tx total   33 u32 MAC tx retry
 *  37  u32 MAC rx total   41 u32 MAC rx errors (no-frame, unknown nbr,
 *                                  invalid src, security, FCS, other)
 *  45  u32 IPv6 rx failure (6LoWPAN reassembly drops)
 *  49  u32 IPv6 tx failure
 *  53  u16 MLE parent changes  55 u16 MLE attach attempts  57 u16 MLE detached
 */
#include <node_mgr/radio_stats.h>
#include <node_mgr/nn_proto_client.h>
#include <nn_proto/nn_proto.h>
#include <nn_osal/osal.h>
#include <nn_pal/openthread.h>

#include <openthread/instance.h>
#include <openthread/link.h>
#include <openthread/thread.h>
#include <string.h>

NN_OSAL_LOG_MODULE(radio_stats);

#ifndef CONFIG_NODE_MGR_RADIO_STATS_PERIOD_MS
#define CONFIG_NODE_MGR_RADIO_STATS_PERIOD_MS 300000
#endif
#define FIRST_REPORT_MS 60000

/* Provided by the patched ESP32 802.15.4 driver; absent elsewhere. */
extern uint32_t nn_esp32_tx_outcome[16] __attribute__((weak));

static nn_osal_work_delayable_t g_dw;
static bool g_started;

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

size_t radio_stats_build(uint8_t *b, size_t len)
{
	if (len < RADIO_STATS_V1_LEN) return 0;
	memset(b, 0, RADIO_STATS_V1_LEN);
	b[0] = 1;
	if (nn_esp32_tx_outcome) {
		const uint32_t *o = nn_esp32_tx_outcome;
		b[1] |= 1;
		nn_osal_put_le32(o[0], b + 13);                 /* ok */
		nn_osal_put_le32(o[1 + 1], b + 17);             /* CCA busy */
		nn_osal_put_le32(o[1 + 3], b + 21);             /* no ack */
		nn_osal_put_le32(o[1 + 2] + o[1 + 4] + o[1 + 5] + o[1 + 6], b + 25);
	}
	otInstance *ot = (otInstance *)nn_pal_ot_instance();
	nn_pal_ot_mutex_lock();
	b[2] = otLinkGetChannel(ot);
	b[3] = (uint8_t)otThreadGetDeviceRole(ot);
	otRouterInfo parent;
	int8_t rssi = 127;
	if (otThreadGetParentInfo(ot, &parent) == OT_ERROR_NONE) {
		put16(b + 4, parent.mRloc16);
		(void)otThreadGetParentAverageRssi(ot, &rssi);
		b[7] = parent.mLinkQualityIn;
		b[8] = parent.mLinkQualityOut;
	} else {
		put16(b + 4, 0xFFFF);
	}
	b[6] = (uint8_t)rssi;
	const otMacCounters *mc = otLinkGetCounters(ot);
	nn_osal_put_le32(mc->mTxTotal, b + 29);
	nn_osal_put_le32(mc->mTxRetry, b + 33);
	nn_osal_put_le32(mc->mRxTotal, b + 37);
	nn_osal_put_le32(mc->mRxErrNoFrame + mc->mRxErrUnknownNeighbor +
			 mc->mRxErrInvalidSrcAddr + mc->mRxErrSec +
			 mc->mRxErrFcs + mc->mRxErrOther, b + 41);
	const otIpCounters *ic = otThreadGetIp6Counters(ot);
	nn_osal_put_le32(ic->mRxFailure, b + 45);
	nn_osal_put_le32(ic->mTxFailure, b + 49);
	const otMleCounters *ml = otThreadGetMleCounters(ot);
	put16(b + 53, ml->mParentChanges);
	put16(b + 55, ml->mAttachAttempts);
	put16(b + 57, ml->mDetachedRole);
	nn_pal_ot_mutex_unlock();
	nn_osal_put_le32(nn_osal_uptime_ms_32() / 1000u, b + 9);
	return RADIO_STATS_V1_LEN;
}

static void radio_stats_fire(nn_osal_work_delayable_t *dw)
{
	uint8_t body[RADIO_STATS_V1_LEN];
	size_t n = radio_stats_build(body, sizeof body);
	/* Unsigned and fire-and-forget: cumulative counters make a lost
	 * report harmless, and no signature means no ~1 s of ECDSA per
	 * report on the device. */
	int rv = nn_proto_client_send_d2h_reply_ns(NN_PROTO_CMD_RADIO_STATS, 0, body, n);
	if (rv) NN_LOG_DBG("RADIO_STATS send: %d", rv);
	nn_osal_work_schedule(dw, CONFIG_NODE_MGR_RADIO_STATS_PERIOD_MS);
}

int radio_stats_start(void)
{
	if (g_started) return 0;
	nn_osal_work_delayable_init(&g_dw, radio_stats_fire);
	int rv = nn_osal_work_schedule(&g_dw, FIRST_REPORT_MS);
	if (rv < 0) return rv;
	g_started = true;
	NN_LOG_INF("radio stats every %d s", CONFIG_NODE_MGR_RADIO_STATS_PERIOD_MS / 1000);
	return 0;
}

void radio_stats_stop(void)
{
	if (!g_started) return;
	(void)nn_osal_work_cancel_delayable(&g_dw);
	g_started = false;
}
