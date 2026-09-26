/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gw_ble_prov — gateway BLE provisioning protocol (platform-neutral).
 *
 * This module owns:
 *   - the 8 characteristic UUIDs and the protocol state machine
 *   - the staged-provisioning buffer (RAM-only until COMMIT)
 *   - ECIES decrypt dispatch + plaintext schema validation
 *   - the commit step that hands the staged blob to gw_provision_set()
 *
 * The GATT server and advertising lifecycle live in a platform-specific
 * backend (Zephyr `bt_gatt_*` on the ESP32-C6 gateway, BlueZ D-Bus on the
 * Linux gateway daemon).  Backends implement `struct gw_ble_prov_backend`
 * and pump events into this module via gw_ble_prov_write_* / read_* /
 * do_decrypt / do_apply.
 *
 * Threading model
 *
 *   gw_ble_prov never spawns its own threads.  It relies on the backend
 *   to serialize calls:
 *     - All write_*() / read_*() / do_decrypt() / do_apply() must run
 *       from a single "protocol" context — for Zephyr that is the BT
 *       RX thread + workqueue; for Linux that is the sd-bus mainloop +
 *       a single decrypt thread.  The backend guarantees mutual
 *       exclusion.
 *     - notify_status() and the schedule_* callbacks may be invoked
 *       from inside gw_ble_prov code (synchronously) — backends must
 *       cope with re-entrant scheduling.
 *
 * Status byte (single value used in STATUS read + notify):
 *   0x00 IDLE
 *   0x01 APPLYING
 *   0x02 SUCCESS
 *   0x03 ERROR_VALIDATION
 *   0x04 ERROR_INCOMPLETE
 *   0x05 ERROR_NVS
 *   0x10 STAGED_WIFI
 *   0x11 STAGED_HUB_HOST
 *   0x12 STAGED_HUB_ID
 *   0x13 STAGED_OT_DATASET
 *   0x40 | s   PSA_FAIL (s = PSA stage index, diagnostic)
 */

#ifndef FW_COMMON_GW_BLE_PROV_H_
#define FW_COMMON_GW_BLE_PROV_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── UUIDs ─────────────────────────────────────────────────────────────
 *
 * Stored in Bluetooth byte order (little-endian 128-bit).  Each backend
 * has its own UUID type (Zephyr `struct bt_uuid_128`, BlueZ string form
 * "e7f01001-…"); both can be derived from these raw bytes.
 */
extern const uint8_t GW_BLE_PROV_SVC_UUID[16];
extern const uint8_t GW_BLE_PROV_WIFI_CRED_UUID[16];
extern const uint8_t GW_BLE_PROV_HUB_HOST_UUID[16];
extern const uint8_t GW_BLE_PROV_HUB_IDENTITY_UUID[16];
extern const uint8_t GW_BLE_PROV_INFO_UUID[16];
extern const uint8_t GW_BLE_PROV_STATUS_UUID[16];
extern const uint8_t GW_BLE_PROV_COMMIT_UUID[16];
extern const uint8_t GW_BLE_PROV_HUB_X25519_UUID[16];
extern const uint8_t GW_BLE_PROV_OT_DATASET_UUID[16];

/* String form of the same UUIDs, for BlueZ D-Bus paths.  Lowercase. */
extern const char GW_BLE_PROV_SVC_UUID_STR[];
extern const char GW_BLE_PROV_WIFI_CRED_UUID_STR[];
extern const char GW_BLE_PROV_HUB_HOST_UUID_STR[];
extern const char GW_BLE_PROV_HUB_IDENTITY_UUID_STR[];
extern const char GW_BLE_PROV_INFO_UUID_STR[];
extern const char GW_BLE_PROV_STATUS_UUID_STR[];
extern const char GW_BLE_PROV_COMMIT_UUID_STR[];
extern const char GW_BLE_PROV_HUB_X25519_UUID_STR[];
extern const char GW_BLE_PROV_OT_DATASET_UUID_STR[];

/* Largest single attribute write — sized for ECIES envelope of the OT
 * operational dataset (largest plaintext, 254 B → ~430 B JSON envelope).
 * +1 for the NUL hub_crypto_decrypt() expects when parsing JSON. */
#define GW_BLE_PROV_ENV_MAX 511

/* GW_INFO payload: [1B schema=2][8B gw_id][65B p256_pub][32B x25519_pub]. */
#define GW_BLE_PROV_INFO_LEN (1 + 8 + 65 + 32)

/* ── Backend ops (protocol → backend) ─────────────────────────────────
 *
 * Implemented by the backend, registered at gw_ble_prov_init().
 */
struct gw_ble_prov_backend {
	/* Push the current STATUS byte as a notification on the STATUS
	 * characteristic.  Backend is responsible for tracking the CCC
	 * state (no-op if the central hasn't subscribed). */
	void (*notify_status)(uint8_t status, void *user);

	/* Ask the backend to schedule deferred ECIES decrypt work.  The
	 * backend MUST eventually call gw_ble_prov_do_decrypt() from its
	 * decrypt thread/workqueue.  Returning quickly from the write
	 * handler is critical on Zephyr to avoid LL supervision timeout
	 * during the 3 s PSA decrypt. */
	void (*schedule_decrypt)(void *user);

	/* Ask the backend to schedule the COMMIT apply work.  Backend
	 * MUST eventually call gw_ble_prov_do_apply().  This may block on
	 * persistent-store I/O (settings_save / file write). */
	void (*schedule_apply)(void *user);

	/* Ask the backend to "reboot" 1 second from now so the SUCCESS
	 * notification has time to clock out.  On Zephyr this is
	 * sys_reboot(SYS_REBOOT_COLD); on the Linux daemon this is
	 * exit(0) (systemd / supervisor restarts us). */
	void (*schedule_reboot_1s)(void *user);

	/* Opaque backend pointer passed back as `user` to all ops. */
	void *user;
};

/* ── Backend → protocol entry points ─────────────────────────────────── */

/* Initialise the protocol module.  `backend` must outlive the module.
 * Computes the gateway's advertising name from `mac6` (last 3 bytes →
 * "nn-gw-XXXXXX") and stashes it for gw_ble_prov_get_name(). */
int  gw_ble_prov_init(const struct gw_ble_prov_backend *backend,
		      const uint8_t mac6[6]);

/* Local advertising name — pointer valid for the lifetime of the
 * module.  Always non-NULL once gw_ble_prov_init() has returned 0. */
const char *gw_ble_prov_get_name(void);

/* Write handlers — call these from the GATT-write paths in the backend.
 * They never block on crypto; they stash the envelope and call the
 * backend's schedule_decrypt op.  Return 0 on accepted, -EBUSY if a
 * previous decrypt is still in flight, -EFBIG if the envelope is
 * oversized, -EINVAL on malformed plaintext writes (HUB_X25519/COMMIT). */
int  gw_ble_prov_write_wifi_cred (const void *buf, size_t len);
int  gw_ble_prov_write_hub_host  (const void *buf, size_t len);
int  gw_ble_prov_write_hub_id    (const void *buf, size_t len);
int  gw_ble_prov_write_ot_dataset(const void *buf, size_t len);

/* HUB_X25519: 32-byte plaintext write — calls hub_crypto_set_hub_pubkey
 * directly, no decrypt. */
int  gw_ble_prov_write_hub_x25519(const void *buf, size_t len);

/* COMMIT: single byte 0x00 (clear) or 0x01 (apply).  On 0x01 the
 * protocol validates that all four staged blobs are present, fires
 * STATUS=APPLYING, and calls schedule_apply. */
int  gw_ble_prov_write_commit    (const void *buf, size_t len);

/* Read handlers — the backend fills its response buffer from these. */
size_t gw_ble_prov_read_gw_info(void *out, size_t cap);
size_t gw_ble_prov_read_status (void *out, size_t cap);

/* Deferred work pumps — backend MUST invoke these after the matching
 * schedule_* op.  Run in any thread; backend serializes. */
void gw_ble_prov_do_decrypt(void);
void gw_ble_prov_do_apply(void);

#ifdef __cplusplus
}
#endif

#endif /* FW_COMMON_GW_BLE_PROV_H_ */
