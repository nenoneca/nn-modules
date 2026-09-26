/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/osal.h>
#include <node_mgr/provision_manager.h>
#include <node_mgr/provision_state.h>
#include <node_mgr/network_manager.h>
#include <node_mgr/hub_crypto.h>

#include <errno.h>
#include <string.h>

#include <nn_pal/ble.h>

NN_OSAL_LOG_MODULE(prov_peripheral);

/* ---------- UUIDs ----------------------------------------------------- */

/*
 * OT Provisioning Service  e7f00001-6b3e-4f6b-9232-3e26d0d5a2f0
 *   Dataset characteristic     e7f00002  (READ|WRITE)
 *   Status characteristic      e7f00003  (READ|NOTIFY)
 *   HUB_CONFIG characteristic  e7f00004  (WRITE)  — [1B name_len][name][32B hub X25519 pub]
 *   DEVICE_PUBKEYS characteristic e7f00005 (READ)  — [32B device X25519 pub]
 *
 * Stored in little-endian wire order so the bytes can be passed to both
 * the PAL UUID parser and an `adv_uuids` advertising blob.
 */
static const nn_pal_ble_uuid_t UUID_PROV_SVC = { .bytes = {
	0xf0,0xa2,0xd5,0xd0,0x26,0x3e,0x32,0x92,
	0x6b,0x4f,0x3e,0x6b, 0x01,0x00,0xf0,0xe7,
}};
static const nn_pal_ble_uuid_t UUID_PROV_DATASET = { .bytes = {
	0xf0,0xa2,0xd5,0xd0,0x26,0x3e,0x32,0x92,
	0x6b,0x4f,0x3e,0x6b, 0x02,0x00,0xf0,0xe7,
}};
static const nn_pal_ble_uuid_t UUID_PROV_STATUS = { .bytes = {
	0xf0,0xa2,0xd5,0xd0,0x26,0x3e,0x32,0x92,
	0x6b,0x4f,0x3e,0x6b, 0x03,0x00,0xf0,0xe7,
}};
static const nn_pal_ble_uuid_t UUID_PROV_HUB_CONFIG = { .bytes = {
	0xf0,0xa2,0xd5,0xd0,0x26,0x3e,0x32,0x92,
	0x6b,0x4f,0x3e,0x6b, 0x04,0x00,0xf0,0xe7,
}};
static const nn_pal_ble_uuid_t UUID_PROV_FW_NAME = { .bytes = {
	0xf0,0xa2,0xd5,0xd0,0x26,0x3e,0x32,0x92,
	0x6b,0x4f,0x3e,0x6b, 0x07,0x00,0xf0,0xe7,
}};
static const nn_pal_ble_uuid_t UUID_PROV_DEV_PUBKEYS = { .bytes = {
	0xf0,0xa2,0xd5,0xd0,0x26,0x3e,0x32,0x92,
	0x6b,0x4f,0x3e,0x6b, 0x05,0x00,0xf0,0xe7,
}};

/* ---------- state ------------------------------------------------------ */

#define DATASET_BUF_MAX 260   /* OT_OPERATIONAL_DATASET_MAX_LENGTH = 254 */
/* Encrypted DATASET_CHAR writes are JSON ECIES v2 envelopes:
 *   {"v":2,"epk":"<b64 32B>","nonce":"<b64 12B>","ct":"<b64 N+16>"}
 * For our 70-byte typical dataset the envelope rounds out to ~200B but
 * we leave headroom for full 254B inner.
 */
#define DATASET_ENV_MAX 512

static uint8_t  dataset_buf[DATASET_BUF_MAX];
static uint16_t dataset_buf_len;

/* M6: incoming write goes here as a JSON envelope.  Workqueue
 * decrypts it into dataset_buf so the BT thread isn't blocked by
 * X25519 ECDH + HKDF + AES-GCM (taking >100 ms here is enough to
 * trip the central's LL supervision timeout — see
 * feedback_zephyr_psa_blocks_bt_thread.md). */
static char     env_buf[DATASET_ENV_MAX + 1];  /* +1 for NUL terminator */
static uint16_t env_buf_len;

#define GW_DECRYPT_WQ_STACK_SIZE 4096
#define GW_DECRYPT_WQ_PRIORITY   11   /* > BT_RX_PRIO (8) → preemptible */
K_THREAD_STACK_DEFINE(s_decrypt_wq_stack, GW_DECRYPT_WQ_STACK_SIZE);
static struct k_work_q s_decrypt_wq;
static struct k_work   s_decrypt_work;
static bool            s_decrypt_wq_started;

static uint8_t  readback_buf[DATASET_BUF_MAX];
static uint16_t readback_len;

#define STATUS_IDLE       0x00
#define STATUS_APPLYING   0x01
#define STATUS_SUCCESS    0x02
#define STATUS_ERROR      0x03

static uint8_t g_status = STATUS_IDLE;

static K_SEM_DEFINE(prov_done_sem, 0, 1);
static bool g_provisioned;
static bool g_adv_started;

static nn_pal_ble_conn_t g_conn;

static struct k_work apply_work;

/* Per-characteristic handles filled in by the PAL after register;
 * captured here so the apply/decrypt work handlers can push notify. */
static nn_pal_ble_chrc_t g_chrcs[5];   /* dataset, status, hub_config, dev_pubkeys, fw_name */
#define CHRC_DATASET     0
#define CHRC_STATUS      1
#define CHRC_HUB_CONFIG  2
#define CHRC_DEV_PUBKEYS 3
#define CHRC_FW_NAME     4

/* ---------- settings handler (via nn_osal_kv) ----------------------- */

static int prov_flag_load(const char *suffix, const uint8_t *value,
			   size_t len, void *user)
{
	ARG_UNUSED(user);
	if (!strcmp(suffix, "done") && len == 1) {
		g_provisioned = (value[0] == 1);
		if (g_provisioned) {
			NN_LOG_INF("BLE provisioning flag loaded");
		}
	}
	return 0;
}

static void save_prov_flag(bool done)
{
	uint8_t val = done ? 1 : 0;
	nn_osal_kv_save("ble_prov/done", &val, sizeof(val));
	g_provisioned = done;
}

/* ---------- hub config state ------------------------------------------ */

/*
 * Device name set via HUB_CONFIG_CHAR.  Stored in NVS by settings.
 * Maximum name length = 32 chars + NUL.
 */
static char  g_hub_name[33];
static bool  g_hub_config_received;

static int hub_name_kv_load(const char *suffix, const uint8_t *value,
			     size_t len, void *user)
{
	ARG_UNUSED(user);
	if (!strcmp(suffix, "name") && len > 0 && len < sizeof(g_hub_name)) {
		memcpy(g_hub_name, value, len);
		g_hub_name[len] = '\0';
		NN_LOG_INF("Hub-provisioned name loaded: %s", g_hub_name);
	}
	return 0;
}

/* ---------- status CCC ------------------------------------------------ */

static void status_ccc_changed(nn_pal_ble_conn_t conn, void *user, bool subscribed)
{
	ARG_UNUSED(conn); ARG_UNUSED(user);
	NN_LOG_INF("Status CCC changed: %s",
		subscribed ? "notify enabled" : "notify disabled");
}

/* ---------- forward declarations -------------------------------------- */

static int write_dataset(const nn_pal_ble_write_ctx_t *ctx, void *user,
			 const uint8_t *buf, size_t len);
static int read_dataset(const nn_pal_ble_read_ctx_t *ctx, void *user,
			uint8_t *out_buf, size_t out_cap, size_t *out_len);
static int read_status(const nn_pal_ble_read_ctx_t *ctx, void *user,
		       uint8_t *out_buf, size_t out_cap, size_t *out_len);
static int write_hub_config(const nn_pal_ble_write_ctx_t *ctx, void *user,
			    const uint8_t *buf, size_t len);
static int read_dev_pubkeys(const nn_pal_ble_read_ctx_t *ctx, void *user,
			    uint8_t *out_buf, size_t out_cap, size_t *out_len);

/* FW_NAME (e7f00007): "<image> <version>", e.g.
 * "nn-app-mdns-ot-esp32c6 0.0.31".  Read by the hub during a provisioning
 * scan so the operator sees WHAT they are about to adopt, and the catalog
 * key is known before the device ever joins the mesh. */
static int read_fw_name(const nn_pal_ble_read_ctx_t *ctx, void *user,
			uint8_t *out_buf, size_t out_cap, size_t *out_len)
{
	(void)ctx; (void)user;
	const char *ver = nn_osal_app_version();
	int n = snprintf((char *)out_buf, out_cap, "%s %s",
			 CONFIG_NODE_MGR_IMAGE_NAME[0] ? CONFIG_NODE_MGR_IMAGE_NAME
						       : "unknown",
			 ver ? ver : "?");
	if (n < 0 || (size_t)n >= out_cap) return -EMSGSIZE;
	*out_len = (size_t)n;
	return 0;
}

/* ---------- async dataset apply --------------------------------------- */

static void notify_status(void)
{
	(void)nn_pal_ble_gatt_notify(g_conn, g_chrcs[CHRC_STATUS].handle,
				     &g_status, sizeof(g_status));
}

static void apply_dataset_work_handler(struct k_work *work)
{
	NN_LOG_INF("Applying dataset (%u bytes)...", dataset_buf_len);

	g_status = STATUS_APPLYING;
	notify_status();

	int rc = nm_thread_apply_dataset(dataset_buf, (uint8_t)dataset_buf_len);
	if (rc != 0) {
		NN_LOG_ERR("Failed to apply dataset: %d", rc);
		g_status = STATUS_ERROR;
	} else {
		save_prov_flag(true);
		(void)prov_state_set(PROV_STATE_PROVISIONED);
		g_status = STATUS_SUCCESS;
		NN_LOG_INF("Provisioning complete — Thread started");
	}

	notify_status();

	if (rc == 0) {
		k_sem_give(&prov_done_sem);
	}
}

/* ---------- GATT callbacks -------------------------------------------- */

static int read_dataset(const nn_pal_ble_read_ctx_t *ctx, void *user,
			uint8_t *out_buf, size_t out_cap, size_t *out_len)
{
	ARG_UNUSED(ctx); ARG_UNUSED(user);
	if (readback_len == 0) return -EPERM;
	if (readback_len > out_cap) return -EMSGSIZE;
	NN_LOG_INF("Dataset read by central: %u bytes", readback_len);
	memcpy(out_buf, readback_buf, readback_len);
	*out_len = readback_len;
	return 0;
}

static int read_status(const nn_pal_ble_read_ctx_t *ctx, void *user,
		       uint8_t *out_buf, size_t out_cap, size_t *out_len)
{
	ARG_UNUSED(ctx); ARG_UNUSED(user);
	if (out_cap < sizeof(g_status)) return -EMSGSIZE;
	out_buf[0] = g_status;
	*out_len   = sizeof(g_status);
	return 0;
}

/* M6: workqueue handler — JSON-decrypts env_buf into dataset_buf,
 * then schedules the apply work.  Runs on a dedicated preemptible
 * queue so the BT thread keeps servicing the LL during the ~hundreds
 * of ms of X25519 ECDH + HKDF + AES-GCM. */
static void decrypt_dataset_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (env_buf_len == 0) {
		NN_LOG_ERR("decrypt: empty envelope");
		g_status = STATUS_ERROR;
		notify_status();
		return;
	}
	env_buf[env_buf_len] = '\0';   /* hub_crypto_decrypt expects NUL-term */

	size_t plain_len = sizeof(dataset_buf);
	int rc = hub_crypto_decrypt(env_buf, dataset_buf, &plain_len);
	if (rc != 0) {
		NN_LOG_ERR("ECIES decrypt failed: %d (env_len=%u) — provisioning aborted",
			rc, env_buf_len);
		g_status = STATUS_ERROR;
		notify_status();
		return;
	}
	dataset_buf_len = (uint16_t)plain_len;
	NN_LOG_INF("Dataset decrypted: %u bytes (from %u-byte envelope) — scheduling apply",
		dataset_buf_len, env_buf_len);

	/* Apply on the system workqueue (existing path).  This in turn
	 * starts Thread which collapses the BLE PHY (RF coex) — that
	 * disconnect is expected and handled by the hub-side python. */
	k_work_submit(&apply_work);
}

static int write_dataset(const nn_pal_ble_write_ctx_t *ctx, void *user,
			 const uint8_t *buf, size_t len)
{
	ARG_UNUSED(user);
	uint16_t offset = ctx->offset;

	if (offset + len > DATASET_ENV_MAX) {
		NN_LOG_ERR("DATASET write out of bounds: offset=%u len=%zu",
			offset, len);
		return -EINVAL;
	}

	/* M6: incoming bytes are an ECIES v2 JSON envelope.  Buffer the
	 * fragments (Write Long uses Prepare/Execute) and decrypt only
	 * after the full envelope arrives.  Reject the whole transfer
	 * if the previous decrypt is still running. */
	if (ctx->flags & NN_PAL_BLE_WRITE_FLAG_PREPARE) {
		memcpy(env_buf + offset, buf, len);
		if (offset + len > env_buf_len) {
			env_buf_len = offset + len;
		}
		return 0;
	}

	if (k_work_busy_get(&s_decrypt_work) != 0) {
		NN_LOG_WRN("DATASET write while previous decrypt still running");
		return -ENOBUFS;
	}

	memcpy(env_buf + offset, buf, len);
	env_buf_len = offset + len;

	NN_LOG_INF("DATASET envelope received: %u bytes — scheduling decrypt",
		env_buf_len);
	k_work_submit_to_queue(&s_decrypt_wq, &s_decrypt_work);

	return 0;
}

/* ---------- hub config + device pubkeys callbacks ----------------------- */

static int write_hub_config(const nn_pal_ble_write_ctx_t *ctx, void *user,
			    const uint8_t *buf, size_t len)
{
	ARG_UNUSED(user);

	if (ctx->offset != 0) {
		return -EINVAL;
	}

	/*
	 * Wire format: [1B name_len][name UTF-8 ≤32 B][32B hub X25519 pub]
	 * Minimum length: 1 (name_len) + 0 (name) + 32 (hub X25519) = 33 bytes
	 */
	if (len < 33) {
		NN_LOG_ERR("HUB_CONFIG write too short: %zu bytes", len);
		return -EMSGSIZE;
	}

	const uint8_t *p = buf;
	uint8_t name_len = p[0];

	if (name_len > 32 || 1 + name_len + 32 > len) {
		NN_LOG_ERR("HUB_CONFIG: invalid name_len %u", name_len);
		return -EMSGSIZE;
	}

	/* Extract name */
	memcpy(g_hub_name, p + 1, name_len);
	g_hub_name[name_len] = '\0';
	nn_osal_kv_save("hub_prov/name", g_hub_name, name_len + 1);
	/* Also save as the app-level device name so main.c picks it up on boot */
	nn_osal_kv_save("mesh_app/name", g_hub_name, name_len + 1);

	/* Extract hub X25519 public key (32 bytes after name) */
	const uint8_t *hub_x25519_pub = p + 1 + name_len;
	hub_crypto_set_hub_pubkey(hub_x25519_pub);

	g_hub_config_received = true;
	NN_LOG_INF("HUB_CONFIG received: name='%s'  hub_x25519=%02x%02x%02x%02x...",
		g_hub_name,
		hub_x25519_pub[0], hub_x25519_pub[1],
		hub_x25519_pub[2], hub_x25519_pub[3]);

	return 0;
}

static int read_dev_pubkeys(const nn_pal_ble_read_ctx_t *ctx, void *user,
			    uint8_t *out_buf, size_t out_cap, size_t *out_len)
{
	ARG_UNUSED(ctx); ARG_UNUSED(user);

	if (out_cap < 32) return -EMSGSIZE;

	/*
	 * Wire format: [32B device X25519 pub]
	 * hub_crypto_init() must be called before advertising starts.
	 */
	hub_crypto_get_device_x25519_pub(out_buf);
	*out_len = 32;

	NN_LOG_INF("DEVICE_PUBKEYS read: X25519=%02x%02x%02x%02x...",
		out_buf[0], out_buf[1], out_buf[2], out_buf[3]);
	return 0;
}

/* ---------- advertising data ------------------------------------------ */

static const nn_pal_ble_adv_params_t g_adv_params = {
	.connectable     = true,
	.interval_ms_min = 30,
	.interval_ms_max = 60,
	.service_uuid    = &UUID_PROV_SVC,
};

/* ---------- connection callbacks -------------------------------------- */

static void conn_event_cb(nn_pal_ble_conn_t conn,
			  nn_pal_ble_conn_event_t ev,
			  const nn_pal_ble_conn_info_t *info,
			  void *user)
{
	ARG_UNUSED(user);
	switch (ev) {
	case NN_PAL_BLE_CONN_EV_CONNECTED:
		NN_LOG_INF("Peripheral: Central connected (mtu=%u)",
			info ? info->mtu : 0);
		g_conn = conn;
		break;
	case NN_PAL_BLE_CONN_EV_DISCONNECTED:
		NN_LOG_INF("Peripheral: disconnected (reason %u)",
			info ? info->disconnect_reason : 0);
		g_conn = 0;
		/* Zephyr stops advertising on connection acceptance.  If the
		 * broker disconnected before delivering the dataset (e.g. it
		 * probed during a leader-fetch retry), restart advertising so
		 * it can come back. */
		if (g_adv_started && !g_provisioned) {
			int err = nn_pal_ble_advertise_start(&g_adv_params);
			if (err && err != -EALREADY) {
				NN_LOG_WRN("Advertising restart failed: %d", err);
			} else {
				NN_LOG_INF("Advertising restarted after disconnect");
			}
		}
		break;
	default:
		break;
	}
}

/* ---------- public API ------------------------------------------------ */

void prov_set_leader_dataset(const uint8_t *tlvs, uint8_t len)
{
	if (!tlvs || len == 0 || len > DATASET_BUF_MAX) {
		return;
	}
	memcpy(readback_buf, tlvs, len);
	readback_len = len;
	NN_LOG_INF("Leader dataset set for read-back: %u bytes", len);
}

int prov_peripheral_start(void)
{
	/* Register kv handlers (idempotent — safe to call on every start). */
	(void)nn_osal_kv_register("ble_prov", prov_flag_load, NULL);
	(void)nn_osal_kv_register("hub_prov", hub_name_kv_load, NULL);

	k_work_init(&apply_work, apply_dataset_work_handler);

	/* M6: dedicated preemptible workqueue for the X25519 decrypt.
	 * Priority 11 is below CONFIG_BT_RX_PRIO (8) so the BT RX thread
	 * always wins — keeps the LL responsive during the hundreds-of-ms
	 * ECDH+HKDF+AEAD path. */
	if (!s_decrypt_wq_started) {
		k_work_init(&s_decrypt_work, decrypt_dataset_work_handler);
		k_work_queue_init(&s_decrypt_wq);
		k_work_queue_start(&s_decrypt_wq, s_decrypt_wq_stack,
				   K_THREAD_STACK_SIZEOF(s_decrypt_wq_stack),
				   GW_DECRYPT_WQ_PRIORITY, NULL);
		s_decrypt_wq_started = true;
	}

	/* Register the GATT service via PAL.  Each chrc's `handle` field
	 * gets populated for later use in nn_pal_ble_gatt_notify(). */
	g_chrcs[CHRC_DATASET] = (nn_pal_ble_chrc_t){
		.uuid     = UUID_PROV_DATASET,
		.flags    = NN_PAL_BLE_CHRC_READ | NN_PAL_BLE_CHRC_WRITE,
		.on_read  = read_dataset,
		.on_write = write_dataset,
	};
	g_chrcs[CHRC_STATUS] = (nn_pal_ble_chrc_t){
		.uuid    = UUID_PROV_STATUS,
		.flags   = NN_PAL_BLE_CHRC_READ | NN_PAL_BLE_CHRC_NOTIFY,
		.on_read = read_status,
		.on_ccc  = status_ccc_changed,
	};
	g_chrcs[CHRC_HUB_CONFIG] = (nn_pal_ble_chrc_t){
		.uuid     = UUID_PROV_HUB_CONFIG,
		.flags    = NN_PAL_BLE_CHRC_WRITE,
		.on_write = write_hub_config,
	};
	g_chrcs[CHRC_DEV_PUBKEYS] = (nn_pal_ble_chrc_t){
		.uuid    = UUID_PROV_DEV_PUBKEYS,
		.flags   = NN_PAL_BLE_CHRC_READ,
		.on_read = read_dev_pubkeys,
	};
	g_chrcs[CHRC_FW_NAME] = (nn_pal_ble_chrc_t){
		.uuid    = UUID_PROV_FW_NAME,
		.flags   = NN_PAL_BLE_CHRC_READ,
		.on_read = read_fw_name,
	};
	nn_pal_ble_service_t svc = {
		.uuid       = UUID_PROV_SVC,
		.chrcs      = g_chrcs,
		.chrc_count = ARRAY_SIZE(g_chrcs),
	};
	int err = nn_pal_ble_gatt_register_service(&svc);
	if (err && err != -EALREADY) {
		NN_LOG_ERR("PAL register_service: %d", err);
		return err;
	}

	(void)nn_pal_ble_conn_cb_register(conn_event_cb, NULL);

	err = nn_pal_ble_advertise_start(&g_adv_params);
	if (err) {
		NN_LOG_ERR("Advertising start failed: %d", err);
		return err;
	}
	g_adv_started = true;
	NN_LOG_INF("BLE advertising started");
	return 0;
}

void prov_peripheral_stop(void)
{
	nn_pal_ble_advertise_stop();
}

int prov_peripheral_wait(uint32_t timeout_ms)
{
	return k_sem_take(&prov_done_sem, K_MSEC(timeout_ms));
}

bool prov_is_provisioned(void)
{
	return g_provisioned;
}

void prov_clear_flag(void)
{
	save_prov_flag(false);
}
