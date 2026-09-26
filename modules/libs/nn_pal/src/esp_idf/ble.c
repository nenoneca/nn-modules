/* SPDX-License-Identifier: Apache-2.0 */
/* nn_pal/ble.h — ESP-IDF (NimBLE) backend, peripheral path.
 *
 * Builds NimBLE's ble_gatt_svc_def tree from the declarative nn_pal service
 * table, routes the single NimBLE access callback to the per-characteristic
 * nn_pal read/write callbacks, and maps GAP events onto nn_pal_ble_conn_cb.
 * Covers exactly what nn_prov (the media provisioning peripheral) needs; the
 * central/scan/GATT-client surface is stubbed (-ENOSYS) until a media use case
 * needs it. */
#if defined(CONFIG_NN_PAL_BACKEND_ESP_IDF)

#include <nn_pal/ble.h>
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_pal_ble);
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

#define MAX_SVC   2
#define MAX_CHR   8

static int gap_event_trampoline(struct ble_gap_event *ev, void *arg);
static bool s_host_started;

static char     s_dev_name[32] = "nn-device";
static uint8_t  s_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

static nn_pal_ble_conn_cb_t s_conn_cb;
static void                *s_conn_user;

/* Saved advertising config so GAP events can re-arm advertising. */
static bool                 s_adv_active;
static ble_uuid128_t        s_adv_svc_uuid;
static bool                 s_adv_has_uuid;

/* Registered service mirror: NimBLE structures + a back-map to the nn_pal
 * characteristics so the access dispatcher can find the user callbacks. */
static ble_uuid128_t        s_svc_uuid[MAX_SVC];
static ble_uuid128_t        s_chr_uuid[MAX_CHR];
static struct ble_gatt_chr_def s_chr_def[MAX_CHR + 1];   /* +1 terminator */
static struct ble_gatt_svc_def s_svc_def[MAX_SVC + 1];
static uint16_t             s_chr_val_handle[MAX_CHR];
static nn_pal_ble_chrc_t   *s_chr_nn[MAX_CHR];
static unsigned             s_chr_count;

/* nn_pal UUIDs are big-endian (spec order); NimBLE ble_uuid128 is LSB-first. */
static void uuid_to_nimble(const nn_pal_ble_uuid_t *in, ble_uuid128_t *out)
{
    out->u.type = BLE_UUID_TYPE_128;
    for (int i = 0; i < 16; i++) out->value[i] = in->bytes[15 - i];
}

int nn_pal_ble_uuid_from_str(nn_pal_ble_uuid_t *out, const char *s)
{
    if (!out || !s) return -EINVAL;
    int bi = 0;
    for (const char *p = s; *p && bi < 16; ) {
        if (*p == '-') { p++; continue; }
        char hex[3] = { p[0], p[1], 0 };
        out->bytes[bi++] = (uint8_t)strtol(hex, NULL, 16);
        p += 2;
    }
    return bi == 16 ? 0 : -EINVAL;
}

/* ── lifecycle ──────────────────────────────────────────────────── */

/* Copy the value handles NimBLE assigned back into the caller's chrc structs.
 *
 * ble_gatts_add_svcs() only QUEUES a service definition — it does not register
 * it, so the val_handle pointers it was given are still zero when it returns.
 * NimBLE fills them in ble_gatts_register_chr(), reached from ble_gatts_start()
 * inside ble_hs_start(), which runs immediately before ble_hs_sync() calls this
 * sync_cb.  Reading the handles any earlier yields 0, and a notify on handle 0
 * is silently dropped: the peripheral looks healthy, the central subscribes
 * happily, and no notification is ever delivered.  That is exactly how every
 * camera provisioning job came back UNCONFIRMED — the hub saw neither APPLYING
 * nor SUCCESS because nn_prov's status notify went to handle 0 every time. */
static void publish_chr_handles(void)
{
    for (unsigned i = 0; i < s_chr_count; i++)
        if (s_chr_nn[i]) s_chr_nn[i]->handle = s_chr_val_handle[i];
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &s_addr_type) != 0) {
        NN_LOG_ERR("no BLE address");
        return;
    }
    publish_chr_handles();
    if (s_adv_active) nn_pal_ble_advertise_start(NULL);  /* re-arm with saved cfg */
}
static void on_reset(int reason) { NN_LOG_WRN("BLE host reset: %d", reason); }
static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

int nn_pal_ble_init(void)
{
    if (nimble_port_init() != ESP_OK) return -EIO;
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    return 0;
}

int nn_pal_ble_shutdown(void) { return nimble_port_stop() == 0 ? 0 : -EIO; }
bool nn_pal_ble_is_ready(void) { return ble_hs_synced(); }

int nn_pal_ble_set_device_name(const char *name)
{
    if (!name) return -EINVAL;
    strlcpy(s_dev_name, name, sizeof s_dev_name);
    ble_svc_gap_device_name_set(s_dev_name);
    return 0;
}

/* nn_pal_ble_conn_t is documented as "0 = invalid", and callers rely on it
 * (`if (!s_conn) return;`).  NimBLE, however, hands out conn handle 0 to the
 * FIRST connection — which is the only connection a provisioning peripheral
 * ever has.  Passing the raw handle up therefore made every caller treat a
 * live central as "not connected", and nn_prov's status notify returned early
 * every single time: no APPLYING, no SUCCESS, and every camera provisioning
 * job stuck at UNCONFIRMED.  Confirmed on air with btmon (2026-08-21) — the
 * central subscribed to the CCCD and the device sent no notification at all.
 *
 * The Zephyr backend already honours the contract by handing out index+1, so
 * do the same here: bias by one at the backend boundary and unbias before
 * calling NimBLE.  Shared code above the PAL then works on both backends. */
#define CONN_TO_PAL(h)   ((nn_pal_ble_conn_t)((h) + 1))
#define CONN_FROM_PAL(c) ((uint16_t)((c) - 1))

/* ── GATT server ────────────────────────────────────────────────── */

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    unsigned idx = (unsigned)(uintptr_t)arg;
    if (idx >= s_chr_count) return BLE_ATT_ERR_UNLIKELY;
    nn_pal_ble_chrc_t *c = s_chr_nn[idx];

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (!c->on_read) return BLE_ATT_ERR_READ_NOT_PERMITTED;
        uint8_t buf[256]; size_t n = 0;
        nn_pal_ble_read_ctx_t rc = { .conn = CONN_TO_PAL(conn), .offset = 0 };
        if (c->on_read(&rc, c->user, buf, sizeof buf, &n) != 0) return BLE_ATT_ERR_UNLIKELY;
        return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (!c->on_write) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        uint8_t buf[600];
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof buf) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &len) != 0) return BLE_ATT_ERR_UNLIKELY;
        nn_pal_ble_write_ctx_t wc = { .conn = CONN_TO_PAL(conn), .offset = 0, .flags = 0 };
        return c->on_write(&wc, c->user, buf, len) < 0 ? BLE_ATT_ERR_UNLIKELY : 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

int nn_pal_ble_gatt_register_service(nn_pal_ble_service_t *svc)
{
    if (!svc || svc->chrc_count > MAX_CHR) return -EINVAL;
    static unsigned svc_i;
    if (svc_i >= MAX_SVC) return -ENOMEM;

    uuid_to_nimble(&svc->uuid, &s_svc_uuid[svc_i]);
    unsigned base = s_chr_count;
    for (unsigned i = 0; i < svc->chrc_count; i++) {
        nn_pal_ble_chrc_t *c = &svc->chrcs[i];
        unsigned gi = base + i;
        uuid_to_nimble(&c->uuid, &s_chr_uuid[gi]);
        s_chr_nn[gi] = c;
        ble_gatt_chr_flags f = 0;
        if (c->flags & NN_PAL_BLE_CHRC_READ)   f |= BLE_GATT_CHR_F_READ;
        if (c->flags & NN_PAL_BLE_CHRC_WRITE)  f |= BLE_GATT_CHR_F_WRITE;
        if (c->flags & NN_PAL_BLE_CHRC_NOTIFY) f |= BLE_GATT_CHR_F_NOTIFY;
        if (c->flags & NN_PAL_BLE_CHRC_INDICATE) f |= BLE_GATT_CHR_F_INDICATE;
        s_chr_def[gi] = (struct ble_gatt_chr_def){
            .uuid = &s_chr_uuid[gi].u,
            .access_cb = access_cb,
            .arg = (void *)(uintptr_t)gi,
            .flags = f,
            .val_handle = &s_chr_val_handle[gi],
        };
    }
    s_chr_count = base + svc->chrc_count;
    memset(&s_chr_def[s_chr_count], 0, sizeof s_chr_def[0]);   /* terminator */

    s_svc_def[svc_i] = (struct ble_gatt_svc_def){
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid[svc_i].u,
        .characteristics = &s_chr_def[base],
    };
    memset(&s_svc_def[svc_i + 1], 0, sizeof s_svc_def[0]);     /* terminator */

    int rc = ble_gatts_count_cfg(s_svc_def);
    if (rc) { NN_LOG_ERR("gatts_count_cfg: %d", rc); return -EIO; }
    rc = ble_gatts_add_svcs(s_svc_def);
    if (rc) { NN_LOG_ERR("gatts_add_svcs: %d", rc); return -EIO; }

    /* These are still 0 until the host registers the service (see
     * publish_chr_handles); copy them anyway so a service registered AFTER
     * sync — when the handles are already valid — is correct immediately. */
    for (unsigned i = 0; i < svc->chrc_count; i++)
        svc->chrcs[i].handle = s_chr_val_handle[base + i];
    svc_i++;
    return 0;
}

int nn_pal_ble_gatt_notify(nn_pal_ble_conn_t conn, uint16_t chrc_handle,
                           const void *data, size_t len)
{
    /* conn is the biased nn_pal handle (see CONN_TO_PAL): 0 means "none". */
    if (conn == 0) return -ENOTCONN;
    /* Attribute handle 0 is never valid.  NimBLE drops such a notify without
     * complaint, which cost a long hunt once already — say so out loud. */
    if (chrc_handle == 0) {
        NN_LOG_ERR("notify on handle 0 — characteristic not registered yet");
        return -EINVAL;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return -ENOMEM;
    int rc = ble_gatts_notify_custom(CONN_FROM_PAL(conn), chrc_handle, om);
    if (rc) NN_LOG_WRN("notify(handle=%u) failed: %d", chrc_handle, rc);
    return rc == 0 ? 0 : -EIO;
}

/* ── advertising ────────────────────────────────────────────────── */

int nn_pal_ble_advertise_start(const nn_pal_ble_adv_params_t *p)
{
    if (p) {  /* NULL = re-arm with the saved config (from on_sync) */
        s_adv_has_uuid = (p->service_uuid != NULL);
        if (s_adv_has_uuid) uuid_to_nimble(p->service_uuid, &s_adv_svc_uuid);
    }
    s_adv_active = true;
    /* First external call: services are registered, so start the NimBLE host
     * task now.  It drives the controller to sync, then on_sync() re-arms
     * advertising with the saved config. */
    if (!s_host_started) {
        s_host_started = true;
        nimble_port_freertos_init(host_task);
        return 0;
    }
    if (!ble_hs_synced()) return 0;   /* on_sync re-arms once the stack is up */

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    if (s_adv_has_uuid) {
        fields.uuids128 = &s_adv_svc_uuid;
        fields.num_uuids128 = 1;
        fields.uuids128_is_complete = 1;
    }
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) { NN_LOG_ERR("adv_set_fields: %d", rc); return -EIO; }

    struct ble_hs_adv_fields sr = { 0 };
    sr.name = (uint8_t *)s_dev_name;
    sr.name_len = strlen(s_dev_name);
    sr.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&sr);

    struct ble_gap_adv_params adv = { 0 };
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event_trampoline, NULL);
    if (rc) { NN_LOG_ERR("adv_start: %d", rc); return -EIO; }
    NN_LOG_INF("advertising as '%s'", s_dev_name);
    return 0;
}

int nn_pal_ble_advertise_stop(void)
{
    s_adv_active = false;
    ble_gap_adv_stop();
    return 0;
}

/* ── GAP events → nn_pal_ble_conn_cb ────────────────────────────── */

static void emit_conn(uint16_t nimble_conn, nn_pal_ble_conn_event_t ev,
                      const nn_pal_ble_conn_info_t *info)
{
    if (s_conn_cb) s_conn_cb(CONN_TO_PAL(nimble_conn), ev, info, s_conn_user);
}

static int gap_event_trampoline(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    nn_pal_ble_conn_info_t info = { 0 };
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn_handle = ev->connect.conn_handle;
            emit_conn(s_conn_handle, NN_PAL_BLE_CONN_EV_CONNECTED, &info);
        } else if (s_adv_active) {
            nn_pal_ble_advertise_start(NULL);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        info.disconnect_reason = ev->disconnect.reason;
        emit_conn(s_conn_handle, NN_PAL_BLE_CONN_EV_DISCONNECTED, &info);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (s_adv_active) nn_pal_ble_advertise_start(NULL);
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_adv_active) nn_pal_ble_advertise_start(NULL);
        return 0;
    case BLE_GAP_EVENT_MTU:
        info.mtu = ev->mtu.value;
        emit_conn(ev->mtu.conn_handle, NN_PAL_BLE_CONN_EV_MTU_UPDATED, &info);
        return 0;
    default:
        return 0;
    }
}

int nn_pal_ble_conn_cb_register(nn_pal_ble_conn_cb_t cb, void *user)
{
    s_conn_cb = cb;
    s_conn_user = user;
    return 0;
}

int nn_pal_ble_conn_disconnect(nn_pal_ble_conn_t conn)
{
    if (conn == 0) return -ENOTCONN;
    return ble_gap_terminate(CONN_FROM_PAL(conn),
                             BLE_ERR_REM_USER_CONN_TERM) == 0 ? 0 : -EIO;
}

/* ── central / scan / GATT-client — not used by the media peripheral ─── */
int nn_pal_ble_connect(const uint8_t a[6], uint8_t t) { (void)a; (void)t; return -ENOSYS; }
int nn_pal_ble_scan_start(const nn_pal_ble_scan_params_t *p, nn_pal_ble_scan_cb_t c, void *u) { (void)p;(void)c;(void)u; return -ENOSYS; }
int nn_pal_ble_scan_stop(void) { return -ENOSYS; }
int nn_pal_ble_gatt_mtu_exchange(nn_pal_ble_conn_t c) { (void)c; return -ENOSYS; }
int nn_pal_ble_gatt_discover(nn_pal_ble_conn_t c, const nn_pal_ble_uuid_t *s, nn_pal_ble_remote_chrc_t *ch, size_t n, nn_pal_ble_disco_cb_t cb, void *u) { (void)c;(void)s;(void)ch;(void)n;(void)cb;(void)u; return -ENOSYS; }
int nn_pal_ble_gatt_read(nn_pal_ble_conn_t c, uint16_t h, nn_pal_ble_gatt_read_cb_t cb, void *u) { (void)c;(void)h;(void)cb;(void)u; return -ENOSYS; }
int nn_pal_ble_gatt_write(nn_pal_ble_conn_t c, uint16_t h, const void *d, size_t n, nn_pal_ble_gatt_write_cb_t cb, void *u) { (void)c;(void)h;(void)d;(void)n;(void)cb;(void)u; return -ENOSYS; }
int nn_pal_ble_gatt_subscribe(nn_pal_ble_conn_t c, uint16_t v, uint16_t cc, nn_pal_ble_gatt_notify_cb_t cb, void *u) { (void)c;(void)v;(void)cc;(void)cb;(void)u; return -ENOSYS; }
int nn_pal_ble_set_pairing_mode(nn_pal_ble_pairing_mode_t m) { (void)m; return 0; }

#endif /* CONFIG_NN_PAL_BACKEND_ESP_IDF */
