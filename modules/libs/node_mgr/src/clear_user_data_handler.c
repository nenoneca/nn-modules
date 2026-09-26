/* SPDX-License-Identifier: Apache-2.0 */

/*
 * clear_user_data_handler.c — H2D CLEAR_USER_DATA responder + boot erase.
 *
 * Wire format (mirrors FIELD_OP_S sealing exactly):
 *   H2D CLEAR_USER_DATA     = [cmd:2|tid:4|sealed]
 *       sealed = ctr(8 BE) || AES-256-GCM(k_h2d, AAD = cmd(2 LE)||tid(4 LE))
 *       plaintext = [ver:1 = 0x01][op:1 = 0x01]   (0x01 = arm clear-on-boot)
 *   D2H CLEAR_USER_DATA_ACK = [cmd:2|tid:4|sealed]
 *       plaintext = [status:1]                     (0 = armed & persisted)
 *
 * Ack-then-wait: the handler ONLY persists a 1-byte flag and acks.  The
 * hub sends REBOOT separately; the erase runs in
 * clear_user_data_boot_check() before Thread auto-attach.  Arming twice
 * is a no-op, which is what makes hub retries safe.
 */

#include <nn_osal/osal.h>
#include <nn_osal/system.h>
#include <node_mgr/nn_proto_client.h>
#include <node_mgr/nn_session_boot.h>
#include <node_mgr/clear_user_data_handler.h>
#include <node_mgr/provision_state.h>
#include <fw_common/nn_session.h>
#include <nn_proto/nn_proto.h>

#include <errno.h>
#include <string.h>

#ifdef CONFIG_NN_OSAL_BACKEND_ZEPHYR
#include <zephyr/net/openthread.h>
#include <openthread.h>          /* openthread_mutex_lock/unlock (non-deprecated API) */
#include <openthread/instance.h>
#endif

NN_OSAL_LOG_MODULE(clear_ud);

#define KV_CLEAR_PREFIX "clear_ud"
#define KV_CLEAR_FLAG   "clear_ud/on_boot"

/* nn_osal_kv has no point read — registration loads the subtree.  Own
 * prefix (not prov_state's: one prefix, one owner) captured at register
 * time; the handler only ever WRITES the flag. */
static bool s_flag_armed;

static int clear_kv_cb(const char *suffix, const uint8_t *value, size_t len,
		       void *user)
{
	ARG_UNUSED(user);
	if (strcmp(suffix, "on_boot") == 0 && len == 1 && value[0] == 1) {
		s_flag_armed = true;
	}
	return 0;
}

static void on_clear_user_data(uint32_t tid,
			       const uint8_t *body, size_t body_len,
			       void *user)
{
	ARG_UNUSED(user);

	uint8_t aad[6];
	nn_osal_put_le16(NN_PROTO_CMD_CLEAR_USER_DATA, aad);
	nn_osal_put_le32(tid, aad + 2);

	uint8_t plain[8];
	size_t  plen = 0;
	int orc = nn_session_boot_open(aad, sizeof aad, body, body_len,
				       plain, sizeof plain, &plen);
	if (orc == -EEXIST) {
		/* Replay of a frame we already served: the flag is armed,
		 * just re-ack so the hub's retry loop completes. */
		NN_LOG_INF("CLEAR_USER_DATA replay tid=0x%08x — re-ack", tid);
	} else if (orc != 0) {
		/* Not sealed under our session ⇒ not the hub.  Drop
		 * silently: an unauthenticated peer learns nothing. */
		NN_LOG_WRN("CLEAR_USER_DATA open rv=%d — drop", orc);
		return;
	} else if (plen != 2 || plain[0] != 0x01 || plain[1] != 0x01) {
		NN_LOG_WRN("CLEAR_USER_DATA bad body (%zu B) — drop", plen);
		return;
	}

	uint8_t status = 0;
	if (orc == 0) {
		uint8_t one = 1;
		int rv = nn_osal_kv_save(KV_CLEAR_FLAG, &one, sizeof one);
		if (rv) {
			NN_LOG_ERR("arm clear flag: kv_save rv=%d", rv);
			status = (uint8_t)(-rv & 0xff);
		} else {
			NN_LOG_INF("clear-on-boot ARMED (tid=0x%08x) — "
				   "awaiting hub REBOOT", tid);
		}
	}

	nn_osal_put_le16(NN_PROTO_CMD_CLEAR_USER_DATA_ACK, aad);
	nn_osal_put_le32(tid, aad + 2);
	uint8_t sealed[1 + NN_SESSION_OVERHEAD];
	size_t  slen = 0;
	if (nn_session_boot_seal(aad, sizeof aad, &status, 1,
				 sealed, sizeof sealed, &slen) != 0) {
		NN_LOG_WRN("CLEAR_USER_DATA_ACK seal failed");
		return;
	}
	(void)nn_proto_client_send_d2h_reply(NN_PROTO_CMD_CLEAR_USER_DATA_ACK,
					     tid, sealed, slen);
}

int clear_user_data_handler_start(void)
{
	return nn_proto_client_register_h2d_handler(
		NN_PROTO_CMD_CLEAR_USER_DATA, on_clear_user_data, NULL);
}

/* ── boot-time erase ────────────────────────────────────────────────── */

bool clear_user_data_boot_check(void)
{
	(void)nn_osal_kv_register(KV_CLEAR_PREFIX, clear_kv_cb, NULL);
	if (!s_flag_armed) {
		return false;
	}

	NN_LOG_INF("clear-on-boot flag set — erasing user data");

	/* Thread first: dataset + network keys.  MANUAL_START means the
	 * stack is initialized but not started at this point, which is
	 * exactly when ErasePersistentInfo is legal. */
#ifdef CONFIG_NN_OSAL_BACKEND_ZEPHYR
	struct openthread_context *otc = openthread_get_default_context();
	if (otc != NULL) {
		openthread_mutex_lock();
		otError oe = otInstanceErasePersistentInfo(openthread_get_default_instance());
		openthread_mutex_unlock();
		NN_LOG_INF("OT persistent info erase: %d", (int)oe);
	}
#endif

	/* User data + (by operator decision 2026-08-20) the device
	 * IDENTITY keys: a re-provisioned device gets a brand-new
	 * device_id, and the hub's archive entry stays a permanent
	 * historical record that can never collide. */
	static const char *keys[] = {
		"hub_crypto/hub_x25519_pub",
		"hub_crypto/dev_x25519_priv",
		"nn_proto_dev/p256_priv",
		"hub_prov/name",
		"mesh_app/name",
		"ble_prov/done",
		"ota/armed_ver",
		"ota/auto_apply",
		"ota/hub_addr",
	};
	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		(void)nn_osal_kv_delete(keys[i]);   /* absent is fine */
	}

	(void)prov_state_set(PROV_STATE_SETUP);

	/* Flag last: a power cut anywhere above re-runs this whole
	 * function on the next boot — every step tolerates repetition. */
	(void)nn_osal_kv_delete(KV_CLEAR_FLAG);

	/* Reboot once more.  The identity keys were loaded into RAM (hub_crypto,
	 * nn_proto_client) BEFORE this check runs, so this boot still holds the
	 * OLD identity while NVS no longer does: provisioning now would register
	 * the old device_id, and the next reboot would mint fresh keys -- a new
	 * id the hub has never seen (c6-s2, 2026-09-26).  The next boot finds no
	 * keys, generates and SAVES them before anything uses them, and comes up
	 * in setup mode (no flag, prov state SETUP). */
	NN_LOG_INF("user data cleared — rebooting so fresh identity keys are made and saved");
	nn_osal_sleep_ms(200);          /* let the log line drain */
	nn_osal_sys_reboot(NN_OSAL_REBOOT_COLD);
	return true;                    /* not reached */
}
