/* SPDX-License-Identifier: Apache-2.0 */

/*
 * ota_hint_handler.c — H2D OTA_HINT + OTA_APPLY responders.
 *
 * Wire format:
 *   H2D OTA_HINT     = [cmd:2 | tid:4]                (body empty)
 *   D2H OTA_HINT_ACK = [cmd:2 | tid:4 | u8 status]    (0=accepted, !=0 errno)
 *   H2D OTA_APPLY    = [cmd:2 | tid:4]                (body empty)
 *     → device replies OTA_HINT_ACK with the same tid, then reboots
 *       ~500 ms later so the ACK can leave the radio first.
 *
 * The hint worker runs check → download → ARM: the downloaded +
 * verified image waits in slot 1 until the hub's operator-confirmed
 * OTA_APPLY arrives (fleet-coordinated reboot).  Set the `auto_apply`
 * KV (mesh ota auto-apply) to restore the old standalone behaviour of
 * applying immediately after download.
 */

#include <nn_osal/osal.h>
#include <node_mgr/nn_proto_client.h>
#include <node_mgr/ota_client.h>
#include <node_mgr/ota_hint_handler.h>

#include <errno.h>
#include <string.h>


#include <nn_proto/nn_proto.h>

NN_OSAL_LOG_MODULE(ota_hint);

/* Dedicated workqueue — OTA download blocks for minutes and would
 * starve the system workqueue (which BT/network rely on).  Preemptible
 * priority so PSA / BT work can still run during long flash writes.
 *
 * 4 KB triggered FATAL ERROR 2 (stack overflow) on c6-s1 OTA before
 * ota_client.c moved its large buffers (`resp[512]`, `reply[~292]`)
 * to a dedicated K_HEAP.  Keep the workqueue at 8 KB for safety
 * headroom against deep OT / flash_img call stacks. */
#define OTA_HINT_WQ_STACK   8192
#define OTA_HINT_WQ_PRIO    11
K_THREAD_STACK_DEFINE(ota_hint_wq_stack, OTA_HINT_WQ_STACK);
static struct k_work_q  s_ota_hint_wq;
static struct k_work    s_ota_hint_work;
static atomic_t         s_in_flight = ATOMIC_INIT(0);

static void ota_hint_worker(struct k_work *w)
{
	ARG_UNUSED(w);

	NN_LOG_INF("hint: check → download → arm");
	int rc = ota_client_check();
	if (rc < 0) {
		NN_LOG_WRN("hint: check failed rc=%d", rc);
		goto done;
	}
	if (rc == 0) {
		NN_LOG_INF("hint: already up to date — nothing to do");
		goto done;
	}

	rc = ota_client_download();
	if (rc < 0) {
		NN_LOG_WRN("hint: download failed rc=%d", rc);
		goto done;
	}

	if (ota_client_get_auto_apply()) {
		NN_LOG_INF("hint: auto_apply set — applying now");
		/* apply requests upgrade + sys_reboot — never returns */
		rc = ota_client_apply();
		NN_LOG_WRN("hint: apply returned rc=%d (unexpected)", rc);
	} else {
		rc = ota_client_arm();
		if (rc < 0) {
			NN_LOG_WRN("hint: arm failed rc=%d", rc);
		}
	}

done:
	atomic_set(&s_in_flight, 0);
}

/* ── OTA_APPLY: operator-confirmed fleet apply ─────────────────────── */

static struct k_work_delayable s_ota_apply_dw;

static void ota_apply_worker(struct k_work *w)
{
	ARG_UNUSED(w);
	NN_LOG_INF("OTA_APPLY: applying armed image — rebooting");
	int rc = ota_client_apply();   /* never returns on success */
	NN_LOG_WRN("OTA_APPLY: apply returned rc=%d (unexpected)", rc);
}

static void on_ota_apply(uint32_t tid,
			 const uint8_t *body, size_t body_len,
			 void *user)
{
	ARG_UNUSED(body); ARG_UNUSED(body_len); ARG_UNUSED(user);

	uint8_t status = 0;
	if (ota_client_armed_version()[0] == '\0') {
		NN_LOG_WRN("OTA_APPLY but not armed — replying EINVAL");
		status = (uint8_t)(-EINVAL & 0xff);
	}
	(void)nn_proto_client_send_d2h_reply(NN_PROTO_CMD_OTA_HINT_ACK,
					     tid, &status, 1);
	if (status == 0) {
		/* Give the ACK ~500 ms to leave the radio before the
		 * reset wipes the stack.  Run on our own workqueue so a
		 * busy system queue can't delay the fleet reboot. */
		k_work_schedule_for_queue(&s_ota_hint_wq, &s_ota_apply_dw,
					  K_MSEC(500));
	}
}

/* ── REBOOT: operator-requested plain reboot (no image involved) ────── */

static struct k_work_delayable s_reboot_dw;

static void reboot_worker(struct k_work *w)
{
	ARG_UNUSED(w);
	NN_LOG_INF("REBOOT: operator requested — warm reboot");
	nn_osal_sys_reboot(NN_OSAL_REBOOT_WARM);
}

static void on_reboot(uint32_t tid,
		      const uint8_t *body, size_t body_len,
		      void *user)
{
	ARG_UNUSED(body); ARG_UNUSED(body_len); ARG_UNUSED(user);

	uint8_t status = 0;
	(void)nn_proto_client_send_d2h_reply(NN_PROTO_CMD_REBOOT_ACK,
					     tid, &status, 1);
	/* Same 500 ms grace as OTA_APPLY: the ACK must leave the radio
	 * before the reset wipes the stack.  Own workqueue for the same
	 * reason — a busy system queue must not delay it. */
	k_work_schedule_for_queue(&s_ota_hint_wq, &s_reboot_dw, K_MSEC(500));
}

static void on_ota_hint(uint32_t tid,
			const uint8_t *body, size_t body_len,
			void *user)
{
	ARG_UNUSED(body);
	ARG_UNUSED(user);

	uint8_t status = 0;
	if (body_len != 0) {
		/* Reserved for future params — accept but log */
		NN_LOG_DBG("OTA_HINT body_len=%zu (ignored)", body_len);
	}

	if (!atomic_cas(&s_in_flight, 0, 1)) {
		NN_LOG_WRN("OTA_HINT already in flight — replying EBUSY");
		status = (uint8_t)(-EBUSY & 0xff);
	} else {
		k_work_submit_to_queue(&s_ota_hint_wq, &s_ota_hint_work);
		NN_LOG_INF("OTA_HINT accepted tid=0x%08x", tid);
	}

	(void)nn_proto_client_send_d2h_reply(NN_PROTO_CMD_OTA_HINT_ACK,
					     tid, &status, 1);
}

int ota_hint_handler_start(void)
{
	k_work_queue_init(&s_ota_hint_wq);
	k_work_queue_start(&s_ota_hint_wq, ota_hint_wq_stack,
			   K_THREAD_STACK_SIZEOF(ota_hint_wq_stack),
			   OTA_HINT_WQ_PRIO, NULL);
	k_thread_name_set(&s_ota_hint_wq.thread, "ota_hint_wq");
	k_work_init(&s_ota_hint_work, ota_hint_worker);
	k_work_init_delayable(&s_ota_apply_dw, ota_apply_worker);
	k_work_init_delayable(&s_reboot_dw, reboot_worker);

	int rc = nn_proto_client_register_h2d_handler(NN_PROTO_CMD_OTA_HINT,
						      on_ota_hint, NULL);
	if (rc) return rc;
	rc = nn_proto_client_register_h2d_handler(NN_PROTO_CMD_OTA_APPLY,
						  on_ota_apply, NULL);
	if (rc) return rc;
	return nn_proto_client_register_h2d_handler(NN_PROTO_CMD_REBOOT,
						    on_reboot, NULL);
}
