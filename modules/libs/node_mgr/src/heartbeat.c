/* SPDX-License-Identifier: Apache-2.0 */

#include <node_mgr/heartbeat.h>
#include <node_mgr/nn_session_boot.h>
#include <node_mgr/nn_proto_client.h>
#include <nn_proto/nn_proto.h>

#include <nn_osal/osal.h>

NN_OSAL_LOG_MODULE(heartbeat);

#ifndef CONFIG_NODE_MGR_HEARTBEAT_PERIOD_MS
#define CONFIG_NODE_MGR_HEARTBEAT_PERIOD_MS  30000
#endif

static nn_osal_work_delayable_t g_dw;
static bool                     g_started;

static void heartbeat_fire(nn_osal_work_delayable_t *dw)
{
	/* Session-salt announcement rides this worker (see
	 * nn_session_boot_hello_tick) — must run even when the
	 * heartbeat itself is skipped below. */
	nn_session_boot_hello_tick();

	/* Skip if any other D2H went out recently — it already refreshed
	 * the hub's last_seen, so an extra HEARTBEAT here is wasted mesh
	 * traffic.  This makes HEARTBEAT a backstop: it only fires when
	 * the sensor has been genuinely idle for the configured period. */
	int64_t now    = nn_osal_uptime_ms();
	int64_t last   = nn_proto_client_last_d2h_send_ms();
	uint32_t since = (last > 0) ? (uint32_t)(now - last) : UINT32_MAX;

	if (since < (uint32_t)CONFIG_NODE_MGR_HEARTBEAT_PERIOD_MS) {
		/* Recent traffic covered us — reschedule for the remaining
		 * window relative to that last send, not a fresh full period. */
		uint32_t next = (uint32_t)CONFIG_NODE_MGR_HEARTBEAT_PERIOD_MS - since;
		NN_LOG_DBG("heartbeat: skip (last D2H %u ms ago); recheck in %u ms",
			   (unsigned)since, (unsigned)next);
		nn_osal_work_schedule(dw, next);
		return;
	}

	/* Body: 4B uptime + (when minted) this boot's 8B session salt.
	 * The salt lets a RESTARTED hub — which lost its in-RAM session
	 * table while the device still believes its session is live —
	 * notice the unknown salt and re-run the handshake, instead of
	 * both sides waiting for a device reboot. */
	uint8_t body[12];
	uint32_t up = nn_osal_uptime_ms_32();
	nn_osal_put_le32(up, body);
	size_t blen = 4;
	if (nn_session_boot_salt_raw(body + 4) == 0) {
		blen = 12;
	}

	int rv = nn_proto_client_send_d2h_cmd(NN_PROTO_CMD_DEVICE_HEARTBEAT,
					      body, blen);
	if (rv) {
		/* Not fatal — the hub will just have a stale last_seen until
		 * the next tick.  Log at DBG (silent on the mesh per the
		 * coap_log threshold) so we can correlate via UART if needed. */
		NN_LOG_DBG("send_d2h_cmd(HEARTBEAT): %d", rv);
	}

	/* Re-arm (send_d2h_cmd just bumped last_d2h_send_ms; the next tick
	 * will re-evaluate against that). */
	nn_osal_work_schedule(dw, CONFIG_NODE_MGR_HEARTBEAT_PERIOD_MS);
}

int heartbeat_start(void)
{
	if (g_started) return 0;
	nn_osal_work_delayable_init(&g_dw, heartbeat_fire);
	int rv = nn_osal_work_schedule(&g_dw,
				       CONFIG_NODE_MGR_HEARTBEAT_PERIOD_MS);
	if (rv < 0) {
		NN_LOG_ERR("work_schedule: %d", rv);
		return rv;
	}
	g_started = true;
	NN_LOG_INF("heartbeat armed (%d ms period)",
		   CONFIG_NODE_MGR_HEARTBEAT_PERIOD_MS);
	return 0;
}

void heartbeat_stop(void)
{
	if (!g_started) return;
	(void)nn_osal_work_cancel_delayable(&g_dw);
	g_started = false;
}
