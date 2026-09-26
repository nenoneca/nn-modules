/* SPDX-License-Identifier: Apache-2.0 */

/*
 * time_sync.c — Pull wall-clock time from hub via the nn_proto control
 * plane.
 *
 *   D2H TIME_QUERY (body empty)
 *       ↓
 *   gateway → hub TCP
 *       ↓
 *   H2D TIME_REPLY  (body: u64 LE epoch ms)
 *
 * Local time = nn_osal_uptime_ms() + offset, where
 *   offset = hub_epoch_ms - nn_osal_uptime_ms() at sync time
 * (one-way delay correction = RTT / 2).
 */

#include <node_mgr/time_sync.h>
#include <node_mgr/nn_proto_client.h>

#include <errno.h>
#include <string.h>


#include <nn_osal/osal.h>
#include <nn_proto/nn_proto.h>

NN_OSAL_LOG_MODULE(time_sync);

static int64_t g_offset_ms;     /* hub_epoch_ms - local_uptime_ms */
static int64_t g_last_sync_ms;  /* local uptime at last sync */
static bool    g_valid;

void time_sync_init(void)
{
	g_offset_ms = 0;
	g_last_sync_ms = 0;
	g_valid = false;
}

int64_t time_sync_now_ms(void)
{
	if (!g_valid) return 0;
	return (int64_t)nn_osal_uptime_ms() + g_offset_ms;
}

int64_t time_sync_age_ms(void)
{
	if (!g_valid) return -1;
	return (int64_t)nn_osal_uptime_ms() - g_last_sync_ms;
}

bool time_sync_is_valid(void)
{
	return g_valid;
}

int time_sync_once(void)
{
	if (!nn_proto_client_gateway_known()) {
		NN_LOG_WRN("gateway unknown — wait for GATEWAY_HELLO");
		return -ENETUNREACH;
	}

	int64_t t1 = nn_osal_uptime_ms();

	uint8_t  reply_body[16];
	size_t   reply_len = sizeof reply_body;
	/* Use the backoff-equipped wrapper — single TIME_QUERY can take
	 * multiple mesh attempts under load.  Same default policy as the
	 * hub-side request_h2d: 5 × 8 s + (1+2+4+8) s = ~55 s budget. */
	int rv = nn_proto_client_request_d2h_retry(NN_PROTO_CMD_TIME_QUERY,
						   NULL, 0,
						   NN_PROTO_CMD_TIME_REPLY,
						   reply_body, &reply_len,
						   /*timeout_ms=*/    8000,
						   /*max_attempts=*/  5,
						   /*backoff_base*/   1000,
						   /*backoff_max*/    16000);

	int64_t t3 = nn_osal_uptime_ms();

	if (rv < 0) {
		NN_LOG_WRN("TIME_QUERY: rv=%d", rv);
		return rv;
	}
	if (reply_len < 8) {
		NN_LOG_WRN("TIME_REPLY too short (%zu)", reply_len);
		return -ENODATA;
	}

	uint64_t hub_epoch_ms = nn_osal_get_le64(reply_body);
	int64_t  rtt_ms       = t3 - t1;
	int64_t  one_way_ms   = rtt_ms / 2;
	int64_t  hub_at_t3    = (int64_t)hub_epoch_ms + one_way_ms;

	g_offset_ms    = hub_at_t3 - t3;
	g_last_sync_ms = t3;
	g_valid        = true;

	NN_LOG_INF("Time sync: hub=%llu ms, rtt=%lld ms, offset=%lld ms",
		(unsigned long long)hub_epoch_ms, rtt_ms, g_offset_ms);
	return 0;
}
