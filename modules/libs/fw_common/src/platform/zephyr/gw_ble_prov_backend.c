/* SPDX-License-Identifier: Apache-2.0 */

/*
 * nn_pal GATT backend for fw_common/gw_ble_prov.  Migrated from a
 * direct Zephyr BT_GATT_SERVICE_DEFINE table to nn_pal/ble.h's runtime
 * service-table API.  Same wire format, same workqueue layout for the
 * slow ECIES decrypt, same backend-op semantics — only the abstraction
 * layer changed.
 */

#include <errno.h>
#include <string.h>

#include <esp_mac.h>

#include <nn_osal/osal.h>
#include <nn_pal/ble.h>
#include <fw_common/gw_ble_prov.h>
#include <fw_common/gw_ble_prov_zephyr.h>

NN_OSAL_LOG_MODULE(gw_ble_prov_be);

/* ── UUID values (must match fw_common/gw_ble_prov.h family) ─────────── */

/* Stored as 16-byte little-endian arrays — same order BT_UUID_128_ENCODE
 * produces and what the PAL backend hands to bt_uuid_create. */
#define _U(low4) { \
	0xf0,0xa2,0xd5,0xd0,0x26,0x3e,0x32,0x92, \
	0x6b,0x4f,0x3e,0x6b, (low4) & 0xff, ((low4) >> 8) & 0xff, 0xf0, 0xe7 }
static const nn_pal_ble_uuid_t UUID_PROV_SVC     = { .bytes = _U(0x1001) };
static const nn_pal_ble_uuid_t UUID_WIFI_CRED    = { .bytes = _U(0x1002) };
static const nn_pal_ble_uuid_t UUID_HUB_HOST     = { .bytes = _U(0x1003) };
static const nn_pal_ble_uuid_t UUID_HUB_ID       = { .bytes = _U(0x1004) };
static const nn_pal_ble_uuid_t UUID_INFO         = { .bytes = _U(0x1005) };
static const nn_pal_ble_uuid_t UUID_STATUS       = { .bytes = _U(0x1006) };
static const nn_pal_ble_uuid_t UUID_COMMIT       = { .bytes = _U(0x1007) };
static const nn_pal_ble_uuid_t UUID_HUB_X25519   = { .bytes = _U(0x1008) };
static const nn_pal_ble_uuid_t UUID_OT_DATASET   = { .bytes = _U(0x1009) };
#undef _U

/* ── connection state ─────────────────────────────────────────────────*/

static nn_pal_ble_conn_t s_conn;
static bool              s_status_subscribed;

#define CHRC_WIFI_CRED   0
#define CHRC_HUB_HOST    1
#define CHRC_HUB_ID      2
#define CHRC_INFO        3
#define CHRC_STATUS      4
#define CHRC_COMMIT      5
#define CHRC_HUB_X25519  6
#define CHRC_OT_DATASET  7

static nn_pal_ble_chrc_t s_chrcs[8];

/* ── per-chrc PAL adapters around the fw_common API ──────────────────── */

/* Common write helper: prepare-writes pass through (fragments buffered
 * by Zephyr); only the final, non-prepare write delivers the value to
 * the fw_common handler. */
static int do_encrypted_write(const nn_pal_ble_write_ctx_t *ctx,
                              const uint8_t *buf, size_t len,
                              int (*fn)(const void *, size_t))
{
	if (ctx->flags & NN_PAL_BLE_WRITE_FLAG_PREPARE) return 0;
	if (ctx->offset != 0) return -EINVAL;
	if (fn(buf, len) < 0) return -EMSGSIZE;
	return 0;
}

static int write_wifi_cred(const nn_pal_ble_write_ctx_t *ctx, void *user,
                           const uint8_t *buf, size_t len)
{ ARG_UNUSED(user); return do_encrypted_write(ctx, buf, len, gw_ble_prov_write_wifi_cred); }

static int write_hub_host(const nn_pal_ble_write_ctx_t *ctx, void *user,
                          const uint8_t *buf, size_t len)
{ ARG_UNUSED(user); return do_encrypted_write(ctx, buf, len, gw_ble_prov_write_hub_host); }

static int write_hub_id(const nn_pal_ble_write_ctx_t *ctx, void *user,
                        const uint8_t *buf, size_t len)
{ ARG_UNUSED(user); return do_encrypted_write(ctx, buf, len, gw_ble_prov_write_hub_id); }

static int write_ot_dataset(const nn_pal_ble_write_ctx_t *ctx, void *user,
                            const uint8_t *buf, size_t len)
{ ARG_UNUSED(user); return do_encrypted_write(ctx, buf, len, gw_ble_prov_write_ot_dataset); }

static int write_hub_x25519(const nn_pal_ble_write_ctx_t *ctx, void *user,
                            const uint8_t *buf, size_t len)
{
	ARG_UNUSED(user);
	if (ctx->flags & NN_PAL_BLE_WRITE_FLAG_PREPARE) return 0;
	if (ctx->offset != 0) return -EINVAL;
	int rv = gw_ble_prov_write_hub_x25519(buf, len);
	if (rv == -EINVAL) return -EMSGSIZE;
	if (rv) return -EIO;
	return 0;
}

static int write_commit(const nn_pal_ble_write_ctx_t *ctx, void *user,
                        const uint8_t *buf, size_t len)
{
	ARG_UNUSED(user);
	if (ctx->offset != 0) return -EINVAL;
	(void)gw_ble_prov_write_commit(buf, len);
	return 0;
}

static int read_gw_info(const nn_pal_ble_read_ctx_t *ctx, void *user,
                        uint8_t *out_buf, size_t out_cap, size_t *out_len)
{
	ARG_UNUSED(ctx); ARG_UNUSED(user);
	if (out_cap < GW_BLE_PROV_INFO_LEN) return -EMSGSIZE;
	*out_len = gw_ble_prov_read_gw_info(out_buf, out_cap);
	return 0;
}

static int read_status(const nn_pal_ble_read_ctx_t *ctx, void *user,
                       uint8_t *out_buf, size_t out_cap, size_t *out_len)
{
	ARG_UNUSED(ctx); ARG_UNUSED(user);
	if (out_cap < 1) return -EMSGSIZE;
	gw_ble_prov_read_status(out_buf, 1);
	*out_len = 1;
	return 0;
}

static void status_ccc_changed(nn_pal_ble_conn_t conn, void *user, bool subscribed)
{
	ARG_UNUSED(conn); ARG_UNUSED(user);
	s_status_subscribed = subscribed;
	NN_LOG_INF("STATUS notifications %s",
		subscribed ? "ENABLED" : "DISABLED");
}

/* ── connection callback ────────────────────────────────────────────── */

static void conn_event_cb(nn_pal_ble_conn_t conn,
                          nn_pal_ble_conn_event_t ev,
                          const nn_pal_ble_conn_info_t *info, void *user)
{
	ARG_UNUSED(user);
	switch (ev) {
	case NN_PAL_BLE_CONN_EV_CONNECTED:
		NN_LOG_INF("BLE connected (mtu=%u)", info ? info->mtu : 0);
		if (!s_conn) s_conn = conn;
		break;
	case NN_PAL_BLE_CONN_EV_DISCONNECTED:
		NN_LOG_INF("BLE disconnected (reason 0x%02x); restarting adv",
			info ? info->disconnect_reason : 0);
		s_conn = 0;
		s_status_subscribed = false;
		{
			nn_pal_ble_adv_params_t p = {
				.connectable     = true,
				.interval_ms_min = 30,
				.interval_ms_max = 60,
				.service_uuid    = &UUID_PROV_SVC,
			};
			int rv = nn_pal_ble_advertise_start(&p);
			if (rv && rv != -EALREADY) NN_LOG_WRN("adv restart: %d", rv);
		}
		break;
	default:
		break;
	}
}

/* ── workqueue for the slow PSA decrypt ───────────────────────────────*/

#define GW_DECRYPT_WQ_STACK_SIZE 4096
#define GW_DECRYPT_WQ_PRIORITY   11
K_THREAD_STACK_DEFINE(s_fw_gwprov_decrypt_wq_stack, GW_DECRYPT_WQ_STACK_SIZE);
static struct k_work_q s_decrypt_wq;

static void decrypt_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	gw_ble_prov_do_decrypt();
}
static K_WORK_DEFINE(s_decrypt_work, decrypt_work_handler);

static void apply_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	gw_ble_prov_do_apply();
}
static K_WORK_DEFINE(s_apply_work, apply_work_handler);

static void reboot_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	nn_osal_sys_reboot(NN_OSAL_REBOOT_COLD);
}
static K_WORK_DELAYABLE_DEFINE(s_reboot_work, reboot_work_handler);

/* ── backend ops ──────────────────────────────────────────────────────*/

static void be_notify_status(uint8_t status, void *user)
{
	ARG_UNUSED(user);
	if (s_conn && s_status_subscribed) {
		(void)nn_pal_ble_gatt_notify(s_conn,
		                             s_chrcs[CHRC_STATUS].handle,
		                             &status, sizeof(status));
	}
}

static void be_schedule_decrypt(void *user)
{
	ARG_UNUSED(user);
	k_work_submit_to_queue(&s_decrypt_wq, &s_decrypt_work);
}

static void be_schedule_apply(void *user)
{
	ARG_UNUSED(user);
	k_work_submit(&s_apply_work);
}

static void be_schedule_reboot_1s(void *user)
{
	ARG_UNUSED(user);
	k_work_schedule(&s_reboot_work, K_MSEC(1000));
}

/* ── public API ───────────────────────────────────────────────────────*/

static bool s_started;

int gw_ble_prov_zephyr_start(void)
{
	if (s_started) return 0;

	k_work_queue_init(&s_decrypt_wq);
	k_work_queue_start(&s_decrypt_wq, s_fw_gwprov_decrypt_wq_stack,
			   K_THREAD_STACK_SIZEOF(s_fw_gwprov_decrypt_wq_stack),
			   GW_DECRYPT_WQ_PRIORITY, NULL);

	uint8_t mac[6];
	esp_err_t merr = esp_efuse_mac_get_default(mac);
	if (merr != 0) {
		NN_LOG_WRN("efuse MAC read: %d (using zero MAC)", (int)merr);
		memset(mac, 0, sizeof mac);
	}

	struct gw_ble_prov_backend be = {
		.notify_status      = be_notify_status,
		.schedule_decrypt   = be_schedule_decrypt,
		.schedule_apply     = be_schedule_apply,
		.schedule_reboot_1s = be_schedule_reboot_1s,
		.user               = NULL,
	};
	int rv = gw_ble_prov_init(&be, mac);
	if (rv) {
		NN_LOG_ERR("gw_ble_prov_init: %d", rv);
		return rv;
	}

	const char *name = gw_ble_prov_get_name();

	rv = nn_pal_ble_init();
	if (rv && rv != -EALREADY) {
		NN_LOG_ERR("nn_pal_ble_init: %d", rv);
		return rv;
	}
	(void)nn_pal_ble_set_device_name(name);

	s_chrcs[CHRC_WIFI_CRED] = (nn_pal_ble_chrc_t){
		.uuid     = UUID_WIFI_CRED,
		.flags    = NN_PAL_BLE_CHRC_WRITE,
		.on_write = write_wifi_cred,
	};
	s_chrcs[CHRC_HUB_HOST] = (nn_pal_ble_chrc_t){
		.uuid     = UUID_HUB_HOST,
		.flags    = NN_PAL_BLE_CHRC_WRITE,
		.on_write = write_hub_host,
	};
	s_chrcs[CHRC_HUB_ID] = (nn_pal_ble_chrc_t){
		.uuid     = UUID_HUB_ID,
		.flags    = NN_PAL_BLE_CHRC_WRITE,
		.on_write = write_hub_id,
	};
	s_chrcs[CHRC_INFO] = (nn_pal_ble_chrc_t){
		.uuid    = UUID_INFO,
		.flags   = NN_PAL_BLE_CHRC_READ,
		.on_read = read_gw_info,
	};
	s_chrcs[CHRC_STATUS] = (nn_pal_ble_chrc_t){
		.uuid    = UUID_STATUS,
		.flags   = NN_PAL_BLE_CHRC_READ | NN_PAL_BLE_CHRC_NOTIFY,
		.on_read = read_status,
		.on_ccc  = status_ccc_changed,
	};
	s_chrcs[CHRC_COMMIT] = (nn_pal_ble_chrc_t){
		.uuid     = UUID_COMMIT,
		.flags    = NN_PAL_BLE_CHRC_WRITE,
		.on_write = write_commit,
	};
	s_chrcs[CHRC_HUB_X25519] = (nn_pal_ble_chrc_t){
		.uuid     = UUID_HUB_X25519,
		.flags    = NN_PAL_BLE_CHRC_WRITE,
		.on_write = write_hub_x25519,
	};
	s_chrcs[CHRC_OT_DATASET] = (nn_pal_ble_chrc_t){
		.uuid     = UUID_OT_DATASET,
		.flags    = NN_PAL_BLE_CHRC_WRITE,
		.on_write = write_ot_dataset,
	};
	nn_pal_ble_service_t svc = {
		.uuid       = UUID_PROV_SVC,
		.chrcs      = s_chrcs,
		.chrc_count = ARRAY_SIZE(s_chrcs),
	};
	rv = nn_pal_ble_gatt_register_service(&svc);
	if (rv && rv != -EALREADY) {
		NN_LOG_ERR("PAL register_service: %d", rv);
		return rv;
	}

	(void)nn_pal_ble_conn_cb_register(conn_event_cb, NULL);

	nn_pal_ble_adv_params_t adv = {
		.connectable     = true,
		.interval_ms_min = 30,
		.interval_ms_max = 60,
		.service_uuid    = &UUID_PROV_SVC,
	};
	rv = nn_pal_ble_advertise_start(&adv);
	if (rv && rv != -EALREADY) {
		NN_LOG_ERR("nn_pal_ble_advertise_start: %d", rv);
		return rv;
	}

	NN_LOG_INF("BLE provisioning advertising as '%s' (svc e7f01001-…)", name);
	s_started = true;
	return 0;
}

int gw_ble_prov_zephyr_stop(void)
{
	if (!s_started) return 0;
	int rv = nn_pal_ble_advertise_stop();
	if (rv && rv != -EALREADY) NN_LOG_WRN("nn_pal_ble_advertise_stop: %d", rv);
	if (s_conn) {
		(void)nn_pal_ble_conn_disconnect(s_conn);
		s_conn = 0;
	}
	rv = nn_pal_ble_shutdown();
	if (rv) NN_LOG_WRN("nn_pal_ble_shutdown: %d", rv);
	else    NN_LOG_INF("BLE stack disabled — RAM reclaimed");
	s_started = false;
	return rv;
}
