/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>


#include <nn_osal/osal.h>
#include <node_mgr/coap_field.h>
#include <node_mgr/field_relay.h>
#include <node_mgr/nn_proto_client.h>
#include <node_mgr/nn_session_boot.h>
#include <fw_common/nn_session.h>
#include <nn_proto/nn_proto.h>

NN_OSAL_LOG_MODULE(field_relay);

#define FIELD_INNER_HDR_LEN  6   /* 2 (cmd) + 4 (tid) */

#ifndef CONFIG_NODE_MGR_FIELD_RELAY_ENV_BUF
#define CONFIG_NODE_MGR_FIELD_RELAY_ENV_BUF 768
#endif

/* The dispatch path does ECIES decrypt (~30-50 ms each on ESP32-C6).
 * Running it on nn_proto_client's rx_thread blocks recvfrom and
 * causes the OT-iface inbound socket queue to fill — which appears
 * as a stream of `otRX: net_pkt alloc FAILED` once the queue
 * exhausts the platform's net_pkt pool.  Solution: copy each
 * incoming FIELD_OP into a slot owned by a dedicated workqueue,
 * return immediately from the rx_thread callback, and let the
 * workqueue do the slow crypto.  rx_thread can then drain inbound
 * traffic continuously. */

#ifndef CONFIG_NODE_MGR_FIELD_RELAY_WQ_STACK_SIZE
/* Sized for env_in[768] + env_out[768] + PSA workspace (~1 KB) +
 * send_d2h_reliable frame buffer (~1 KB) + call frames.  4096 hit
 * Zephyr FATAL 2 stack overflow during E2E (commit fffac27).  Bumped
 * 8192 → 12288 after the cross-device cascade path exposed another
 * tight budget: auto_engine.notify_targets → send_d2d_reliable →
 * rel_burst_fire stacks rel_send_one (inner[774]) + send_signed_frame
 * (~1 KB) + PSA sign workspace (~1 KB) on top of the existing
 * env_in/env_out frame, blowing 8 KB during the second notify target's
 * burst-fire round on the field_relay worker thread. */
#define CONFIG_NODE_MGR_FIELD_RELAY_WQ_STACK_SIZE 12288
#endif
#ifndef CONFIG_NODE_MGR_FIELD_RELAY_QUEUE_DEPTH
#define CONFIG_NODE_MGR_FIELD_RELAY_QUEUE_DEPTH 4
#endif

struct field_op_job {
	struct k_work work;
	uint8_t       payload[CONFIG_NODE_MGR_FIELD_RELAY_ENV_BUF +
			      FIELD_INNER_HDR_LEN];
	size_t        len;
	bool          in_use;
};

K_THREAD_STACK_DEFINE(s_field_wq_stack,
		      CONFIG_NODE_MGR_FIELD_RELAY_WQ_STACK_SIZE);
static struct k_work_q s_field_wq;
static bool            s_wq_started;
static K_MUTEX_DEFINE(s_jobs_lock);
static struct field_op_job s_jobs[CONFIG_NODE_MGR_FIELD_RELAY_QUEUE_DEPTH];

static void on_field_reply_acked(nn_proto_reliable_result_t result,
				 uint32_t tid, uint32_t rtt_ms,
				 const uint8_t *reply_body, size_t reply_body_len,
				 void *user)
{
	ARG_UNUSED(user);
	/* FIELD_REPLY_ACK carries no payload; reply_body/_len always 0. */
	ARG_UNUSED(reply_body);
	ARG_UNUSED(reply_body_len);
	if (result == NN_PROTO_RELIABLE_OK) {
		NN_LOG_INF("FIELD_REPLY acked tid=0x%08x (rtt=%u ms)",
			tid, rtt_ms);
	} else {
		NN_LOG_WRN("FIELD_REPLY gave up tid=0x%08x (rv=%d)",
			tid, (int)result);
	}
}

static void field_op_work_handler(struct k_work *w)
{
	struct field_op_job *job = CONTAINER_OF(w, struct field_op_job, work);

	const uint8_t *payload = job->payload;
	size_t len = job->len;

	uint16_t cmd = nn_osal_get_le16(payload);
	uint32_t tid = nn_osal_get_le32(payload + 2);

	/* Hub re-sent a request we have already served?  The reply is
	 * still retransmitting from its reliable slot — let it, and skip
	 * the (expensive) re-serve. */
	if (nn_proto_client_reliable_tid_active(tid)) {
		NN_LOG_INF("FIELD_OP dup tid=0x%08x — reply pending, skip", tid);
		goto done;
	}

	if (cmd == NN_PROTO_CMD_FIELD_OP_S) {
		/* Phase 3 sealed path: NO asymmetric crypto anywhere.
		 * open(session) -> serve -> seal(session) -> reliable
		 * nosign send.  AAD binds cmd+tid each direction. */
		uint8_t aad[6];
		nn_osal_put_le16(NN_PROTO_CMD_FIELD_OP_S, aad);
		nn_osal_put_le32(tid, aad + 2);

		char plain_req[256];
		size_t pr_len = 0;
		int orc = nn_session_boot_open(aad, sizeof aad,
					       payload + FIELD_INNER_HDR_LEN,
					       len - FIELD_INNER_HDR_LEN,
					       (uint8_t *)plain_req,
					       sizeof plain_req - 1, &pr_len);
		if (orc == -EEXIST) {
			NN_LOG_INF("FIELD_OP_S replay tid=0x%08x — skip", tid);
			goto done;
		}
		if (orc != 0) {
			NN_LOG_WRN("FIELD_OP_S open rv=%d (tid=0x%08x)",
				   orc, tid);
			goto done;
		}
		plain_req[pr_len] = '\0';

		char resp_plain[256];
		int  rp_len = 0;
		orc = field_op_dispatch_plain(plain_req, resp_plain,
					      sizeof resp_plain, &rp_len);
		if (orc != 0) {
			NN_LOG_WRN("dispatch_plain rv=%d (tid=0x%08x)",
				   orc, tid);
			goto done;
		}

		nn_osal_put_le16(NN_PROTO_CMD_FIELD_REPLY_S, aad);
		nn_osal_put_le32(tid, aad + 2);
		uint8_t sealed[256 + NN_SESSION_OVERHEAD];
		size_t  sealed_len = 0;
		orc = nn_session_boot_seal(aad, sizeof aad,
					   (const uint8_t *)resp_plain,
					   (size_t)rp_len,
					   sealed, sizeof sealed, &sealed_len);
		if (orc != 0) {
			NN_LOG_WRN("FIELD_REPLY_S seal rv=%d", orc);
			goto done;
		}
		int sr = nn_proto_client_send_d2h_reliable_ns(
			NN_PROTO_CMD_FIELD_REPLY_S,
			NN_PROTO_CMD_FIELD_REPLY_ACK,
			tid, sealed, sealed_len,
			on_field_reply_acked, NULL);
		if (sr) {
			NN_LOG_WRN("FIELD_REPLY_S enqueue rv=%d (tid=0x%08x)",
				   sr, tid);
		} else {
			NN_LOG_INF("FIELD_REPLY_S enqueued tid=0x%08x (%zu B)",
				   tid, sealed_len);
		}
		goto done;
	}

	size_t env_len = len - FIELD_INNER_HDR_LEN;
	char env_in[CONFIG_NODE_MGR_FIELD_RELAY_ENV_BUF];
	memcpy(env_in, payload + FIELD_INNER_HDR_LEN, env_len);
	env_in[env_len] = '\0';

	char env_out[CONFIG_NODE_MGR_FIELD_RELAY_ENV_BUF];
	size_t env_out_len = 0;
	int rc = field_op_dispatch(env_in, env_out, sizeof(env_out),
				   &env_out_len);
	if (rc != 0) {
		NN_LOG_WRN("field_op_dispatch rv=%d (tid=0x%08x)", rc, tid);
		goto done;
	}

	int sr = nn_proto_client_send_d2h_reliable(
		NN_PROTO_CMD_FIELD_REPLY,
		NN_PROTO_CMD_FIELD_REPLY_ACK,
		tid,
		(const uint8_t *)env_out, env_out_len,
		on_field_reply_acked, NULL);
	if (sr) {
		NN_LOG_WRN("FIELD_REPLY enqueue rv=%d (tid=0x%08x)", sr, tid);
		goto done;
	}
	NN_LOG_INF("FIELD_REPLY enqueued tid=0x%08x (%zu B)",
		tid, env_out_len);

done:
	k_mutex_lock(&s_jobs_lock, K_FOREVER);
	job->in_use = false;
	k_mutex_unlock(&s_jobs_lock);
}

static void wq_ensure_started(void)
{
	if (s_wq_started) return;
	k_work_queue_init(&s_field_wq);
	struct k_work_queue_config cfg = { .name = "field_relay" };
	k_work_queue_start(&s_field_wq, s_field_wq_stack,
			   K_THREAD_STACK_SIZEOF(s_field_wq_stack),
			   K_PRIO_PREEMPT(8), &cfg);
	for (int i = 0; i < CONFIG_NODE_MGR_FIELD_RELAY_QUEUE_DEPTH; i++) {
		k_work_init(&s_jobs[i].work, field_op_work_handler);
	}
	s_wq_started = true;
}

static void handle_field_op(const uint8_t *payload, size_t len)
{
	if (len <= FIELD_INNER_HDR_LEN) {
		NN_LOG_WRN("FIELD_OP truncated: %zu B", len);
		return;
	}
	if (len > sizeof(((struct field_op_job *)0)->payload)) {
		NN_LOG_WRN("FIELD_OP too large: %zu B", len);
		return;
	}

	wq_ensure_started();

	/* Grab a free slot, copy the payload, submit work.  If no slot
	 * is free we drop this FIELD_OP — the hub's own retry will
	 * resend it once an earlier decrypt finishes and a slot frees.
	 * This back-pressure is preferred to blocking the rx_thread. */
	k_mutex_lock(&s_jobs_lock, K_FOREVER);
	struct field_op_job *job = NULL;
	for (int i = 0; i < CONFIG_NODE_MGR_FIELD_RELAY_QUEUE_DEPTH; i++) {
		if (!s_jobs[i].in_use) { job = &s_jobs[i]; break; }
	}
	if (!job) {
		k_mutex_unlock(&s_jobs_lock);
		NN_LOG_DBG("FIELD_OP dropped — all %d slots in use",
			   CONFIG_NODE_MGR_FIELD_RELAY_QUEUE_DEPTH);
		return;
	}
	job->in_use = true;
	memcpy(job->payload, payload, len);
	job->len = len;
	k_mutex_unlock(&s_jobs_lock);

	k_work_submit_to_queue(&s_field_wq, &job->work);
}

bool field_relay_try_handle_h2d(const uint8_t *payload, size_t len)
{
	if (len < 2) {
		return false;
	}
	uint16_t cmd = nn_osal_get_le16(payload);
	if (cmd != NN_PROTO_CMD_FIELD_OP && cmd != NN_PROTO_CMD_FIELD_OP_S) {
		return false;
	}
	handle_field_op(payload, len);
	return true;
}
