/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <fw_common/gw_ble_prov.h>
#include <fw_common/gw_identity.h>
#include <fw_common/gw_provision.h>
#include <fw_common/hub_crypto.h>
#include <fw_common/log.h>

LOG_MODULE_REGISTER(gw_ble_prov, LOG_LEVEL_INF);

/* ── UUIDs (Bluetooth little-endian byte order) ───────────────────────
 *
 * Service family: e7f01xxx-6b3e-4f6b-9232-3e26d0d5a2f0
 *   xxx = 1001 service, 1002 wifi_cred, 1003 hub_host, 1004 hub_identity,
 *         1005 gw_info, 1006 status, 1007 commit, 1008 hub_x25519,
 *         1009 ot_dataset.
 *
 * Byte order in the raw arrays matches the wire byte order BlueZ + Zephyr
 * use (LE on the wire) so the same bytes drop into either backend.
 */
#define UUID_FAMILY_TAIL \
	0x32, 0x92, 0x6b, 0x4f, 0x3e, 0x6b, 0xf0, 0xa2, 0xd5, 0xd0, 0x26, 0x3e
/* Wait — the standard 128-bit UUID byte order for BLE attribute values is
 * little-endian.  The Zephyr macro BT_UUID_128_ENCODE(W0, W1, W2, W3, W4)
 * emits the bytes in LE order.  We mirror it here so a raw memcpy into
 * either backend's UUID struct works.
 *
 * For "e7f010xx-6b3e-4f6b-9232-3e26d0d5a2f0" the LE order is:
 *   [f0, a2, d5, d0, 26, 3e, 32, 92, 6b, 4f, 3e, 6b, xx, 10, f0, e7].
 */

#define MAKE_UUID(byte12, byte13)                                          \
	{ 0xf0, 0xa2, 0xd5, 0xd0, 0x26, 0x3e, 0x32, 0x92,                  \
	  0x6b, 0x4f, 0x3e, 0x6b, (byte12), (byte13), 0xf0, 0xe7 }

const uint8_t GW_BLE_PROV_SVC_UUID         [16] = MAKE_UUID(0x01, 0x10);
const uint8_t GW_BLE_PROV_WIFI_CRED_UUID   [16] = MAKE_UUID(0x02, 0x10);
const uint8_t GW_BLE_PROV_HUB_HOST_UUID    [16] = MAKE_UUID(0x03, 0x10);
const uint8_t GW_BLE_PROV_HUB_IDENTITY_UUID[16] = MAKE_UUID(0x04, 0x10);
const uint8_t GW_BLE_PROV_INFO_UUID        [16] = MAKE_UUID(0x05, 0x10);
const uint8_t GW_BLE_PROV_STATUS_UUID      [16] = MAKE_UUID(0x06, 0x10);
const uint8_t GW_BLE_PROV_COMMIT_UUID      [16] = MAKE_UUID(0x07, 0x10);
const uint8_t GW_BLE_PROV_HUB_X25519_UUID  [16] = MAKE_UUID(0x08, 0x10);
const uint8_t GW_BLE_PROV_OT_DATASET_UUID  [16] = MAKE_UUID(0x09, 0x10);

const char GW_BLE_PROV_SVC_UUID_STR        [] = "e7f01001-6b3e-4f6b-9232-3e26d0d5a2f0";
const char GW_BLE_PROV_WIFI_CRED_UUID_STR  [] = "e7f01002-6b3e-4f6b-9232-3e26d0d5a2f0";
const char GW_BLE_PROV_HUB_HOST_UUID_STR   [] = "e7f01003-6b3e-4f6b-9232-3e26d0d5a2f0";
const char GW_BLE_PROV_HUB_IDENTITY_UUID_STR[]= "e7f01004-6b3e-4f6b-9232-3e26d0d5a2f0";
const char GW_BLE_PROV_INFO_UUID_STR       [] = "e7f01005-6b3e-4f6b-9232-3e26d0d5a2f0";
const char GW_BLE_PROV_STATUS_UUID_STR     [] = "e7f01006-6b3e-4f6b-9232-3e26d0d5a2f0";
const char GW_BLE_PROV_COMMIT_UUID_STR     [] = "e7f01007-6b3e-4f6b-9232-3e26d0d5a2f0";
const char GW_BLE_PROV_HUB_X25519_UUID_STR [] = "e7f01008-6b3e-4f6b-9232-3e26d0d5a2f0";
const char GW_BLE_PROV_OT_DATASET_UUID_STR [] = "e7f01009-6b3e-4f6b-9232-3e26d0d5a2f0";

/* ── status byte (mirrors gw_ble_prov.h doc) ──────────────────────────*/

enum {
	STATUS_IDLE             = 0x00,
	STATUS_APPLYING         = 0x01,
	STATUS_SUCCESS          = 0x02,
	STATUS_ERROR_VALIDATION = 0x03,
	STATUS_ERROR_INCOMPLETE = 0x04,
	STATUS_ERROR_NVS        = 0x05,
	STATUS_STAGED_WIFI      = 0x10,
	STATUS_STAGED_HUB_HOST  = 0x11,
	STATUS_STAGED_HUB_ID    = 0x12,
	STATUS_STAGED_OT_DATASET = 0x13,
};

enum decrypt_target {
	DECRYPT_TARGET_NONE     = 0,
	DECRYPT_TARGET_WIFI     = 1,
	DECRYPT_TARGET_HUB_HOST = 2,
	DECRYPT_TARGET_HUB_ID   = 3,
	DECRYPT_TARGET_OT_DATASET = 4,
};

/* ── module state ─────────────────────────────────────────────────────*/

static struct gw_ble_prov_backend s_be;
static char     s_name[16];          /* "nn-gw-XXXXXX" + NUL */
static uint8_t  s_status = STATUS_IDLE;
static bool     s_initialised;

/* In-flight decrypt slot.  The backend serializes write_*() against
 * do_decrypt(), so there's at most one envelope queued at a time. */
static struct {
	enum decrypt_target target;
	uint16_t            len;
	uint8_t             buf[GW_BLE_PROV_ENV_MAX + 1];
	bool                busy;
} s_pending;

static struct {
	bool    have_wifi;
	bool    have_hub_host;
	bool    have_hub_id;
	bool    have_ot_dataset;
	char    ssid[GW_PROVISION_SSID_MAX + 1];
	char    psk[GW_PROVISION_PSK_MAX + 1];
	char    hub_host[GW_PROVISION_HUB_MDNS_MAX + 1];
	uint8_t hub_id[GW_PROVISION_HUB_ID_LEN];
	uint8_t hub_pub[GW_PROVISION_HUB_PUB_LEN];
	uint8_t ot_dataset[GW_PROVISION_OT_DATASET_MAX];
	uint16_t ot_dataset_len;
} s_staged;

static void clear_staged(void)
{
	memset(&s_staged, 0, sizeof s_staged);
}

static void set_status(uint8_t st)
{
	s_status = st;
	if (s_be.notify_status) {
		s_be.notify_status(st, s_be.user);
	}
}

/* ── public init ──────────────────────────────────────────────────────*/

int gw_ble_prov_init(const struct gw_ble_prov_backend *backend,
		     const uint8_t mac6[6])
{
	if (!backend || !mac6) return -EINVAL;
	s_be = *backend;
	int n = snprintf(s_name, sizeof s_name,
			 "nn-gw-%02X%02X%02X",
			 mac6[3], mac6[4], mac6[5]);
	if (n < 0 || n >= (int)sizeof s_name) {
		snprintf(s_name, sizeof s_name, "nn-gw");
	}

	int crv = hub_crypto_init();
	if (crv) LOG_WRN("hub_crypto_init: %d", crv);

	s_initialised = true;
	s_status = STATUS_IDLE;
	clear_staged();
	return 0;
}

const char *gw_ble_prov_get_name(void)
{
	return s_initialised ? s_name : "";
}

/* ── write paths ──────────────────────────────────────────────────────*/

static int stage_decrypt(enum decrypt_target target, const void *buf, size_t len)
{
	if (len > GW_BLE_PROV_ENV_MAX) return -EFBIG;
	if (s_pending.busy) return -EBUSY;
	s_pending.target = target;
	s_pending.len    = (uint16_t)len;
	memcpy(s_pending.buf, buf, len);
	s_pending.busy = true;
	if (s_be.schedule_decrypt) {
		s_be.schedule_decrypt(s_be.user);
	}
	return 0;
}

int gw_ble_prov_write_wifi_cred(const void *buf, size_t len)
{
	int rv = stage_decrypt(DECRYPT_TARGET_WIFI, buf, len);
	if (rv < 0) {
		LOG_WRN("WIFI_CRED busy or oversized (len=%zu rv=%d)", len, rv);
		set_status(STATUS_ERROR_VALIDATION);
	}
	return rv;
}

int gw_ble_prov_write_hub_host(const void *buf, size_t len)
{
	int rv = stage_decrypt(DECRYPT_TARGET_HUB_HOST, buf, len);
	if (rv < 0) {
		LOG_WRN("HUB_HOST busy or oversized (len=%zu rv=%d)", len, rv);
		set_status(STATUS_ERROR_VALIDATION);
	}
	return rv;
}

int gw_ble_prov_write_hub_id(const void *buf, size_t len)
{
	int rv = stage_decrypt(DECRYPT_TARGET_HUB_ID, buf, len);
	if (rv < 0) {
		LOG_WRN("HUB_IDENTITY busy or oversized (len=%zu rv=%d)", len, rv);
		set_status(STATUS_ERROR_VALIDATION);
	}
	return rv;
}

int gw_ble_prov_write_ot_dataset(const void *buf, size_t len)
{
	int rv = stage_decrypt(DECRYPT_TARGET_OT_DATASET, buf, len);
	if (rv < 0) {
		LOG_WRN("OT_DATASET busy or oversized (len=%zu rv=%d)", len, rv);
		set_status(STATUS_ERROR_VALIDATION);
	}
	return rv;
}

int gw_ble_prov_write_hub_x25519(const void *buf, size_t len)
{
	if (len != 32) {
		LOG_WRN("HUB_X25519: bad length %zu (expected 32)", len);
		return -EINVAL;
	}
	int rv = hub_crypto_set_hub_pubkey((const uint8_t *)buf);
	if (rv) {
		LOG_WRN("hub_crypto_set_hub_pubkey: %d", rv);
		return rv;
	}
	const uint8_t *p = buf;
	LOG_INF("HUB_X25519 set: %02x%02x%02x%02x...", p[0], p[1], p[2], p[3]);
	return 0;
}

int gw_ble_prov_write_commit(const void *buf, size_t len)
{
	if (len != 1) return -EINVAL;
	uint8_t magic = ((const uint8_t *)buf)[0];

	if (magic == 0x00) {
		LOG_INF("COMMIT(clear): wiping staged state");
		clear_staged();
		set_status(STATUS_IDLE);
		return 0;
	}
	if (magic != 0x01) {
		return -EINVAL;
	}
	if (!s_staged.have_wifi || !s_staged.have_hub_host ||
	    !s_staged.have_hub_id || !s_staged.have_ot_dataset) {
		LOG_WRN("COMMIT(apply) staged incomplete: wifi=%d host=%d "
			"hub_id=%d ot=%d",
			s_staged.have_wifi, s_staged.have_hub_host,
			s_staged.have_hub_id, s_staged.have_ot_dataset);
		set_status(STATUS_ERROR_INCOMPLETE);
		return -ENODATA;
	}

	LOG_INF("COMMIT(apply): scheduling apply work");
	set_status(STATUS_APPLYING);
	if (s_be.schedule_apply) {
		s_be.schedule_apply(s_be.user);
	}
	return 0;
}

/* ── read paths ───────────────────────────────────────────────────────*/

size_t gw_ble_prov_read_gw_info(void *out, size_t cap)
{
	if (!out || cap < GW_BLE_PROV_INFO_LEN) return 0;
	uint8_t *info = (uint8_t *)out;
	info[0] = 2;
	const uint8_t *id  = gw_identity_get_id();
	const uint8_t *pub = gw_identity_get_pubkey();
	if (id)  memcpy(info + 1, id, GW_PROVISION_HUB_ID_LEN);
	else     memset(info + 1, 0, GW_PROVISION_HUB_ID_LEN);
	if (pub) memcpy(info + 9, pub, GW_PROVISION_HUB_PUB_LEN);
	else     memset(info + 9, 0, GW_PROVISION_HUB_PUB_LEN);
	hub_crypto_get_device_x25519_pub(info + 1 + 8 + 65);
	return GW_BLE_PROV_INFO_LEN;
}

size_t gw_ble_prov_read_status(void *out, size_t cap)
{
	if (!out || cap < 1) return 0;
	((uint8_t *)out)[0] = s_status;
	return 1;
}

/* ── deferred work pumps ──────────────────────────────────────────────*/

void gw_ble_prov_do_decrypt(void)
{
	enum decrypt_target tgt = s_pending.target;
	uint16_t len = s_pending.len;
	if (!s_pending.busy || tgt == DECRYPT_TARGET_NONE) {
		return;
	}

	uint8_t plain[GW_PROVISION_OT_DATASET_MAX];
	size_t  plain_len = sizeof(plain);

	s_pending.buf[len] = '\0';  /* hub_crypto_decrypt wants NUL-terminated JSON */
	int dr = hub_crypto_decrypt((char *)s_pending.buf, plain, &plain_len);

	/* Free the slot before parsing so the backend can accept the next
	 * write while we validate. */
	s_pending.busy = false;
	s_pending.target = DECRYPT_TARGET_NONE;

	if (dr < 0) {
		LOG_WRN("hub_crypto_decrypt fail: tgt=%d rv=%d (env=%u)",
			tgt, dr, len);
		set_status(STATUS_ERROR_VALIDATION);
		return;
	}

	switch (tgt) {
	case DECRYPT_TARGET_WIFI: {
		if (plain_len < 2) goto bad;
		uint8_t ssid_len = plain[0];
		if (ssid_len == 0 || ssid_len > GW_PROVISION_SSID_MAX) goto bad;
		if (plain_len < 1u + ssid_len + 1u) goto bad;
		uint8_t psk_len = plain[1 + ssid_len];
		if (psk_len > GW_PROVISION_PSK_MAX) goto bad;
		if (plain_len != 1u + ssid_len + 1u + psk_len) goto bad;
		memcpy(s_staged.ssid, plain + 1, ssid_len);
		s_staged.ssid[ssid_len] = '\0';
		memcpy(s_staged.psk, plain + 2 + ssid_len, psk_len);
		s_staged.psk[psk_len] = '\0';
		s_staged.have_wifi = true;
		LOG_INF("WIFI_CRED staged: ssid=%u psk=%u", ssid_len, psk_len);
		set_status(STATUS_STAGED_WIFI);
		return;
	}
	case DECRYPT_TARGET_HUB_HOST: {
		if (plain_len < 2) goto bad;
		uint8_t host_len = plain[0];
		if (host_len == 0 || host_len > GW_PROVISION_HUB_MDNS_MAX) goto bad;
		if (plain_len != 1u + host_len) goto bad;
		memcpy(s_staged.hub_host, plain + 1, host_len);
		s_staged.hub_host[host_len] = '\0';
		s_staged.have_hub_host = true;
		LOG_INF("HUB_HOST staged: '%s'", s_staged.hub_host);
		set_status(STATUS_STAGED_HUB_HOST);
		return;
	}
	case DECRYPT_TARGET_HUB_ID: {
		if (plain_len !=
		    GW_PROVISION_HUB_ID_LEN + GW_PROVISION_HUB_PUB_LEN) goto bad;
		if (plain[GW_PROVISION_HUB_ID_LEN] != 0x04) goto bad;
		memcpy(s_staged.hub_id, plain, GW_PROVISION_HUB_ID_LEN);
		memcpy(s_staged.hub_pub, plain + GW_PROVISION_HUB_ID_LEN,
		       GW_PROVISION_HUB_PUB_LEN);
		s_staged.have_hub_id = true;
		LOG_INF("HUB_IDENTITY staged");
		set_status(STATUS_STAGED_HUB_ID);
		return;
	}
	case DECRYPT_TARGET_OT_DATASET: {
		if (plain_len == 0 || plain_len > GW_PROVISION_OT_DATASET_MAX) goto bad;
		memcpy(s_staged.ot_dataset, plain, plain_len);
		s_staged.ot_dataset_len = (uint16_t)plain_len;
		s_staged.have_ot_dataset = true;
		LOG_INF("OT_DATASET staged: %u B", (unsigned)plain_len);
		set_status(STATUS_STAGED_OT_DATASET);
		return;
	}
	case DECRYPT_TARGET_NONE:
		break;
	}
bad:
	LOG_WRN("plaintext failed schema validation (tgt=%d)", tgt);
	set_status(STATUS_ERROR_VALIDATION);
}

void gw_ble_prov_do_apply(void)
{
	int rv = gw_provision_set(s_staged.ssid, s_staged.psk,
				  s_staged.hub_host,
				  s_staged.hub_id, s_staged.hub_pub,
				  s_staged.ot_dataset,
				  s_staged.ot_dataset_len);
	if (rv < 0) {
		LOG_ERR("gw_provision_set: %d", rv);
		set_status(STATUS_ERROR_NVS);
		return;
	}
	LOG_INF("provisioning committed — scheduling reboot in 1 s");
	set_status(STATUS_SUCCESS);
	if (s_be.schedule_reboot_1s) {
		s_be.schedule_reboot_1s(s_be.user);
	}
}
