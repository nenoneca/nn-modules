/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_pal/ble.h — Bluetooth GATT abstraction (peripheral + central).
 *
 * The biggest porting payoff in the PAL set.  Wraps:
 *   - bt_enable / bt_disable
 *   - GATT service registration (declarative table)
 *   - advertise / scan
 *   - connect (central) / connection events (peripheral)
 *   - subscribe + notify
 *   - pairing (currently only Just-Works exposed)
 *
 * Design notes:
 *   - Service definition is TABLE-BASED (array of attributes), not
 *     callback-tree.  Easy to translate to Zephyr's bt_gatt_attr[] AND
 *     to NimBLE's ble_gatt_svc_def tree (the backend builds the tree).
 *   - UUIDs are 128-bit byte arrays.  16-bit / 32-bit shortcuts can be
 *     synthesised by the backend if needed (we don't use SIG-assigned
 *     UUIDs in the nn project).
 *   - Connections are referenced by uint16_t handle, not opaque pointer,
 *     so the API is ABI-stable across backends.
 *   - All callbacks fire from a single backend-managed task; callers
 *     don't have to worry about the BT host's threading model.
 *
 * Apps that today use Zephyr Bluetooth directly (provision_peripheral.c,
 * provision_central.c, gw_ble_prov_backend.c, mdns_ot main.c) all map
 * cleanly onto this surface.
 */

/* ── lifecycle ──────────────────────────────────────────────────── */

int  nn_pal_ble_init(void);
int  nn_pal_ble_shutdown(void);
bool nn_pal_ble_is_ready(void);

/* Set the BLE device name (advertised in scan response).  Persists
 * for the runtime; not stored to NVS by the PAL. */
int nn_pal_ble_set_device_name(const char *name);

/* ── UUIDs ──────────────────────────────────────────────────────── */

typedef struct {
    uint8_t bytes[16];  /* big-endian per BLE spec */
} nn_pal_ble_uuid_t;

/* Convenience: parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" string. */
int nn_pal_ble_uuid_from_str(nn_pal_ble_uuid_t *out, const char *s);

/* ── GATT service definition ──────────────────────────────────── */

#define NN_PAL_BLE_CHRC_READ    (1 << 0)
#define NN_PAL_BLE_CHRC_WRITE   (1 << 1)
#define NN_PAL_BLE_CHRC_NOTIFY  (1 << 2)
#define NN_PAL_BLE_CHRC_INDICATE (1 << 3)

/* Write-context flags — set by the backend before invoking the user's
 * on_write callback.  PREPARE means "this is a Write-Long fragment;
 * accumulate but don't act yet"; the final, unflagged write delivers
 * the assembled value. */
#define NN_PAL_BLE_WRITE_FLAG_PREPARE  (1U << 0)
#define NN_PAL_BLE_WRITE_FLAG_CMD      (1U << 1)

typedef uint16_t nn_pal_ble_conn_t;  /* 0 = invalid */

/* Per-call context handed to read/write callbacks.  Keeps the
 * signature stable as we add fields (e.g. authentication info). */
typedef struct {
    nn_pal_ble_conn_t conn;
    uint16_t          offset;
} nn_pal_ble_read_ctx_t;

typedef struct {
    nn_pal_ble_conn_t conn;
    uint16_t          offset;
    uint32_t          flags;   /* NN_PAL_BLE_WRITE_FLAG_* */
} nn_pal_ble_write_ctx_t;

/* Read callback — backend calls this when a central reads the char.
 * Caller copies up to `out_cap` bytes into `out_buf`, writes the actual
 * length to *out_len.  Return 0 on success; negative errno on failure
 * (backend translates -EPERM → BT_ATT_ERR_READ_NOT_PERMITTED, etc.). */
typedef int (*nn_pal_ble_read_cb_t)(const nn_pal_ble_read_ctx_t *ctx,
                                    void *user,
                                    uint8_t *out_buf, size_t out_cap,
                                    size_t *out_len);

/* Write callback — backend calls this when a central writes the char.
 * Return 0 (or the number of bytes accepted) on success; negative
 * errno to NAK. */
typedef int (*nn_pal_ble_write_cb_t)(const nn_pal_ble_write_ctx_t *ctx,
                                     void *user,
                                     const uint8_t *buf, size_t len);

/* Subscription state change.  For NOTIFY chrcs only; fires when a
 * central writes the CCC descriptor. */
typedef void (*nn_pal_ble_ccc_cb_t)(nn_pal_ble_conn_t conn,
                                    void *user,
                                    bool subscribed);

typedef struct {
    nn_pal_ble_uuid_t       uuid;
    uint32_t                flags;      /* NN_PAL_BLE_CHRC_* */
    nn_pal_ble_read_cb_t    on_read;    /* NULL if READ flag not set */
    nn_pal_ble_write_cb_t   on_write;   /* NULL if WRITE flag not set */
    nn_pal_ble_ccc_cb_t     on_ccc;     /* NULL OK; only for NOTIFY chrcs */
    void                   *user;       /* opaque, passed to cbs */
    /* Filled by backend after register — opaque handle the caller
     * uses to push notifications. */
    uint16_t                handle;
} nn_pal_ble_chrc_t;

typedef struct {
    nn_pal_ble_uuid_t     uuid;
    nn_pal_ble_chrc_t    *chrcs;
    size_t                chrc_count;
} nn_pal_ble_service_t;

/* Register a service (and all its characteristics).
 *
 * Caller-allocated tables — the backend RETAINS THE POINTERS, so the
 * storage must outlive the service.  A compound literal does not
 * satisfy this.  Its lifetime ends with the enclosing statement, so the
 * service is left registered against freed memory; it registers and
 * advertises perfectly and fails only once a central tries to use it,
 * which is how it reached the field and took BLE provisioning down.
 * Use static storage, or an allocation the caller keeps.
 *
 * The host stack may interrogate this application BEFORE the call can
 * complete — asking for the object tree, reading advertisement
 * properties.  A backend must therefore keep servicing the host stack
 * while its own registration request is outstanding: a synchronous
 * request/reply that blocks the servicing thread cannot answer, so the
 * registration never completes.  Callers see only that this may take
 * more than one round trip.
 *
 * Returns 0 on success.  After registration, `chrcs[i].handle` is
 * filled in. */
int nn_pal_ble_gatt_register_service(nn_pal_ble_service_t *svc);

/* Push a notification on a characteristic that has NOTIFY flag.
 * Returns 0 on success, -EINVAL if no subscriber, -ENOTCONN if no
 * active connection. */
int nn_pal_ble_gatt_notify(nn_pal_ble_conn_t conn,
                           uint16_t chrc_handle,
                           const void *data, size_t len);

/* ── advertising ────────────────────────────────────────────────── */

typedef struct {
    /* Advertise as connectable + general-discoverable.  Default true. */
    bool connectable;
    /* Period in ms — backend picks nearest valid interval. */
    uint16_t interval_ms_min;
    uint16_t interval_ms_max;
    /* 128-bit service UUID to include in the adv data so centrals can
     * filter.  Set to NULL to skip. */
    const nn_pal_ble_uuid_t *service_uuid;
} nn_pal_ble_adv_params_t;

int nn_pal_ble_advertise_start(const nn_pal_ble_adv_params_t *p);
int nn_pal_ble_advertise_stop(void);

/* ── scan + central (peripheral apps don't need these) ─────────── */

typedef struct {
    uint8_t  addr[6];      /* 6-byte BLE address, little-endian */
    uint8_t  addr_type;    /* 0=public, 1=random */
    int8_t   rssi_dbm;
    /* Service UUIDs found in adv data; NULL if not parsed. */
    const nn_pal_ble_uuid_t *adv_uuids;
    size_t adv_uuid_count;
} nn_pal_ble_scan_result_t;

typedef void (*nn_pal_ble_scan_cb_t)(const nn_pal_ble_scan_result_t *r,
                                     void *user);

typedef struct {
    /* Active scans transmit SCAN_REQ to fetch scan-response data; passive
     * scans only listen.  Default (passive=true) is what nn uses. */
    bool passive;
} nn_pal_ble_scan_params_t;

int nn_pal_ble_scan_start(const nn_pal_ble_scan_params_t *params,
                          nn_pal_ble_scan_cb_t cb, void *user);
int nn_pal_ble_scan_stop(void);

/* ── connection ─────────────────────────────────────────────────── */

typedef enum {
    NN_PAL_BLE_CONN_EV_CONNECTED    = 1,
    NN_PAL_BLE_CONN_EV_DISCONNECTED = 2,
    NN_PAL_BLE_CONN_EV_MTU_UPDATED  = 3,
    NN_PAL_BLE_CONN_EV_BONDED       = 4,
} nn_pal_ble_conn_event_t;

typedef struct {
    uint16_t  mtu;                 /* current ATT MTU */
    uint8_t   peer_addr[6];
    uint8_t   peer_addr_type;
    int       disconnect_reason;   /* on DISCONNECTED */
} nn_pal_ble_conn_info_t;

typedef void (*nn_pal_ble_conn_cb_t)(nn_pal_ble_conn_t conn,
                                     nn_pal_ble_conn_event_t ev,
                                     const nn_pal_ble_conn_info_t *info,
                                     void *user);

int nn_pal_ble_conn_cb_register(nn_pal_ble_conn_cb_t cb, void *user);

/* Disconnect a specific connection. */
int nn_pal_ble_conn_disconnect(nn_pal_ble_conn_t conn);

/* Central-side connect.  Returns 0 on initiation; the connect-event
 * callback fires when established (or never if timeout). */
int nn_pal_ble_connect(const uint8_t addr[6], uint8_t addr_type);

/* ── GATT client (central) ──────────────────────────────────────── */

/* Exchange ATT MTU on a connection.  Synchronous from the caller's
 * perspective — returns once the exchange completes or fails. */
int nn_pal_ble_gatt_mtu_exchange(nn_pal_ble_conn_t conn);

/* Description of a remote characteristic that the caller wants the
 * backend to find during discovery.  Caller pre-fills `uuid`; the
 * backend writes the handles after a successful discovery. */
typedef struct {
    nn_pal_ble_uuid_t uuid;
    uint16_t value_handle;  /* 0 if not found */
    uint16_t ccc_handle;    /* 0 if not present or chrc lacks NOTIFY */
} nn_pal_ble_remote_chrc_t;

/* Discovery completion callback.  `err` is 0 on success, negative
 * errno on failure (timeout, peer-reject, etc.). */
typedef void (*nn_pal_ble_disco_cb_t)(nn_pal_ble_conn_t conn,
                                       int err, void *user);

/* Discover a primary service + a caller-listed set of characteristics
 * (and their CCC descriptors).  Caller owns `chrcs[]` storage; the
 * backend retains it until the callback fires.  Returns 0 if the
 * discovery sequence was kicked off, negative if it couldn't start. */
int nn_pal_ble_gatt_discover(nn_pal_ble_conn_t conn,
                             const nn_pal_ble_uuid_t *svc_uuid,
                             nn_pal_ble_remote_chrc_t *chrcs,
                             size_t chrc_count,
                             nn_pal_ble_disco_cb_t cb, void *user);

/* Read a remote characteristic.  Backend reassembles long reads; `cb`
 * fires once with the complete value. */
typedef void (*nn_pal_ble_gatt_read_cb_t)(nn_pal_ble_conn_t conn,
                                           int err,
                                           const uint8_t *data, size_t len,
                                           void *user);

int nn_pal_ble_gatt_read(nn_pal_ble_conn_t conn, uint16_t handle,
                         nn_pal_ble_gatt_read_cb_t cb, void *user);

/* Write a remote characteristic.  cb fires once when the peer ACKs
 * (or never if the connection drops). */
typedef void (*nn_pal_ble_gatt_write_cb_t)(nn_pal_ble_conn_t conn,
                                            int err, void *user);

int nn_pal_ble_gatt_write(nn_pal_ble_conn_t conn, uint16_t handle,
                          const void *data, size_t len,
                          nn_pal_ble_gatt_write_cb_t cb, void *user);

/* Subscribe to notifications on a remote characteristic.  Writes
 * 0x0001 to ccc_handle and routes inbound notifications through `cb`
 * (cb returning false stops further notifications). */
typedef bool (*nn_pal_ble_gatt_notify_cb_t)(nn_pal_ble_conn_t conn,
                                             const uint8_t *data, size_t len,
                                             void *user);

int nn_pal_ble_gatt_subscribe(nn_pal_ble_conn_t conn,
                              uint16_t value_handle, uint16_t ccc_handle,
                              nn_pal_ble_gatt_notify_cb_t cb, void *user);

/* ── pairing ────────────────────────────────────────────────────── */

typedef enum {
    /* No I/O — pairing falls back to Just-Works. */
    NN_PAL_BLE_PAIRING_JUST_WORKS = 0,
    /* Future expansion: passkey-display, passkey-entry, OOB. */
    NN_PAL_BLE_PAIRING_PASSKEY_DISPLAY = 1,
    NN_PAL_BLE_PAIRING_PASSKEY_ENTRY   = 2,
} nn_pal_ble_pairing_mode_t;

int nn_pal_ble_set_pairing_mode(nn_pal_ble_pairing_mode_t m);
