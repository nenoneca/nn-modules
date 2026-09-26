/*
 * channel_scan -- see include/node_mgr/channel_scan.h.
 *
 * otLinkEnergyScan is asynchronous: the request handler only starts it;
 * the completion callback (OpenThread context) queues a work item that
 * sends the reply, so no extra thread stack is spent.  While the scan
 * runs the radio is off the mesh channel for 16 x dwell -- the hub
 * staggers devices and keeps the dwell short.
 *
 * The reply is not acked: the hub asks again with the SAME tid when it
 * hears nothing, and a repeat of the last tid is answered from the cached
 * result instead of scanning again.
 */
#include <node_mgr/channel_scan.h>
#include <node_mgr/nn_proto_client.h>
#include <nn_proto/nn_proto.h>
#include <nn_osal/osal.h>
#include <nn_pal/openthread.h>
#include <zephyr/net/openthread.h>   /* openthread_get_default_context, api mutex */
#include <openthread.h>          /* openthread_mutex_lock/unlock (non-deprecated API) */

#include <openthread/instance.h>
#include <openthread/link.h>
#include <string.h>
#include <zephyr/kernel.h>

NN_OSAL_LOG_MODULE(channel_scan);

#define SCAN_MASK_11_26 0x07fff800u

static int8_t   g_max[16];
static uint8_t  g_reply[2 + 16];   /* [status][channel][16 x dBm] */
static uint32_t g_tid;             /* tid of the running / last scan */
static bool     g_busy, g_have;
static struct k_work g_send;

static void send_reply(struct k_work *w)
{
	ARG_UNUSED(w);
	(void)nn_proto_client_send_d2h_reply(NN_PROTO_CMD_CHANNEL_SCAN_REPLY,
					     g_tid, g_reply, sizeof g_reply);
}

static void scan_cb(otEnergyScanResult *r, void *ctx)
{
	ARG_UNUSED(ctx);
	if (r) {
		if (r->mChannel >= 11 && r->mChannel <= 26)
			g_max[r->mChannel - 11] = r->mMaxRssi;
		return;
	}
	/* NULL = scan finished */
	g_reply[0] = 0;
	memcpy(g_reply + 2, g_max, 16);
	g_have = true;
	g_busy = false;
	k_work_submit(&g_send);
}

static void on_req(uint32_t tid, const uint8_t *body, size_t body_len, void *user)
{
	ARG_UNUSED(user);
	if (g_busy) {
		return;                 /* the hub re-asks; answer when done */
	}
	if (g_have && tid == g_tid) {
		k_work_submit(&g_send); /* re-ask: our reply was lost */
		return;
	}
	uint16_t ms = body_len >= 2 ? (uint16_t)(body[0] | (body[1] << 8)) : 200;
	if (ms < 50) ms = 50;
	if (ms > 1000) ms = 1000;

	otInstance *ot = openthread_get_default_instance();
	for (int i = 0; i < 16; i++) g_max[i] = 127;
	g_tid = tid;
	g_have = false;
	g_busy = true;
	openthread_mutex_lock();
	g_reply[1] = otLinkGetChannel(ot);
	otError e = otLinkEnergyScan(ot, SCAN_MASK_11_26, ms, scan_cb, NULL);
	openthread_mutex_unlock();
	if (e != OT_ERROR_NONE) {
		g_busy = false;
		g_reply[0] = (uint8_t)(int8_t)-EIO;
		memset(g_reply + 2, 127, 16);
		g_have = true;
		k_work_submit(&g_send);
		NN_LOG_WRN("CHANNEL_SCAN tid=%u: otLinkEnergyScan %d", tid, e);
		return;
	}
	NN_LOG_INF("CHANNEL_SCAN tid=%u: %u ms per channel", tid, ms);
}

int channel_scan_start(void)
{
	k_work_init(&g_send, send_reply);
	return nn_proto_client_register_h2d_handler(NN_PROTO_CMD_CHANNEL_SCAN_REQ, on_req, NULL);
}
