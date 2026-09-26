/* SPDX-License-Identifier: Apache-2.0 */
/* Implementation of the assumed-pattern fake — see fake_nimble.h. */
#include "fake_nimble.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include <string.h>
#include <stdlib.h>

struct ble_hs_cfg_s ble_hs_cfg;

static bool s_synced;
static bool s_connected;
static ble_gap_event_fn *s_gap_cb;
static void *s_gap_arg;

fake_notify_rec_t fake_notifies[8];
unsigned fake_notify_count;
unsigned fake_notify_dropped;
int fake_last_terminate = -1;
const struct ble_gatt_svc_def *fake_svcs;
bool fake_adv_running;

void fake_nimble_reset(void)
{
    s_synced = s_connected = false;
    s_gap_cb = NULL; s_gap_arg = NULL;
    memset(fake_notifies, 0, sizeof fake_notifies);
    fake_notify_count = fake_notify_dropped = 0;
    fake_last_terminate = -1;
    fake_svcs = NULL;
    fake_adv_running = false;
    memset(&ble_hs_cfg, 0, sizeof ble_hs_cfg);
}

/* — lifecycle — */
int  nimble_port_init(void) { return ESP_OK; }
int  nimble_port_stop(void) { return 0; }
void nimble_port_run(void)  { }
void nimble_port_freertos_init(void (*host_task)(void *)) { (void)host_task; }
void nimble_port_freertos_deinit(void) { }
int  ble_hs_util_ensure_addr(int p) { (void)p; return 0; }
int  ble_hs_id_infer_auto(int p, uint8_t *t) { (void)p; *t = 0; return 0; }
bool ble_hs_synced(void) { return s_synced; }
void ble_svc_gap_init(void) { }
void ble_svc_gatt_init(void) { }
int  ble_svc_gap_device_name_set(const char *n) { (void)n; return 0; }

size_t strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (cap) {
        size_t c = n >= cap ? cap - 1 : n;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

/* — GATT registration: P2, queue only — */
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *defs)
{ (void)defs; return 0; }

int ble_gatts_add_svcs(const struct ble_gatt_svc_def *defs)
{
    fake_svcs = defs;      /* val_handles deliberately NOT assigned here */
    return 0;
}

void fake_nimble_go_sync(void)
{
    /* P2: handles exist only from registration-at-start onward. */
    uint16_t h = 10;
    for (const struct ble_gatt_svc_def *s = fake_svcs;
         s && s->type; s++) {
        for (const struct ble_gatt_chr_def *c = s->characteristics;
             c && c->uuid; c++) {
            if (c->val_handle) *c->val_handle = (uint16_t)(h += 3);
        }
    }
    s_synced = true;
    if (ble_hs_cfg.sync_cb) ble_hs_cfg.sync_cb();
}

/* — mbuf — */
int os_mbuf_append(struct os_mbuf *om, const void *data, uint16_t len)
{
    if (om->len + len > sizeof om->buf) return -1;
    memcpy(om->buf + om->len, data, len);
    om->len += len;
    return 0;
}
struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, uint16_t len)
{
    if (len > 600) return NULL;
    struct os_mbuf *om = calloc(1, sizeof *om);
    if (om) { memcpy(om->buf, data, len); om->len = len; }
    return om;
}
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *buf,
                        uint16_t cap, uint16_t *outlen)
{
    if (om->len > cap) return -1;
    memcpy(buf, om->buf, om->len);
    *outlen = om->len;
    return 0;
}

/* — notify: P3 + P4 — */
int ble_gatts_notify_custom(uint16_t conn_handle, uint16_t chr_val_handle,
                            struct os_mbuf *om)
{
    int rc = 0;
    if (chr_val_handle == 0) {
        fake_notify_dropped++;                 /* P3: silent drop */
    } else if (!s_connected || conn_handle != 0) {
        rc = BLE_HS_ENOTCONN;                  /* P4: raw handle or nothing */
    } else if (fake_notify_count < 8) {
        fake_notify_rec_t *r = &fake_notifies[fake_notify_count++];
        r->conn = conn_handle;
        r->attr = chr_val_handle;
        r->len = om->len;
        memcpy(r->data, om->buf, om->len);
    }
    free(om);
    return rc;
}

/* — GAP — */
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f) { (void)f; return 0; }
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *f) { (void)f; return 0; }
int ble_gap_adv_start(uint8_t t, const void *p, int32_t d,
                      const struct ble_gap_adv_params *ap,
                      ble_gap_event_fn *cb, void *arg)
{
    (void)t; (void)p; (void)d; (void)ap;
    s_gap_cb = cb; s_gap_arg = arg;
    fake_adv_running = true;
    return 0;
}
int ble_gap_adv_stop(void) { fake_adv_running = false; return 0; }

int ble_gap_terminate(uint16_t conn_handle, uint8_t reason)
{
    (void)reason;
    fake_last_terminate = conn_handle;
    /* P4: only the raw handle of the live connection works. */
    if (!s_connected || conn_handle != 0) return BLE_HS_ENOTCONN;
    return 0;
}

void fake_nimble_connect(void)
{
    s_connected = true;
    fake_adv_running = false;
    struct ble_gap_event ev = { .type = BLE_GAP_EVENT_CONNECT };
    ev.connect.status = 0;
    ev.connect.conn_handle = 0;                /* P1: first conn is 0 */
    if (s_gap_cb) s_gap_cb(&ev, s_gap_arg);
}
void fake_nimble_disconnect(int reason)
{
    s_connected = false;
    struct ble_gap_event ev = { .type = BLE_GAP_EVENT_DISCONNECT };
    ev.disconnect.reason = reason;
    if (s_gap_cb) s_gap_cb(&ev, s_gap_arg);
}
void fake_nimble_mtu(uint16_t mtu)
{
    struct ble_gap_event ev = { .type = BLE_GAP_EVENT_MTU };
    ev.mtu.conn_handle = 0;
    ev.mtu.value = mtu;
    if (s_gap_cb) s_gap_cb(&ev, s_gap_arg);
}

/* — driving the access callback the PAL registered — */
static const struct ble_gatt_chr_def *nth_chr(unsigned idx)
{
    unsigned i = 0;
    for (const struct ble_gatt_svc_def *s = fake_svcs; s && s->type; s++)
        for (const struct ble_gatt_chr_def *c = s->characteristics;
             c && c->uuid; c++, i++)
            if (i == idx) return c;
    return NULL;
}
uint16_t fake_attr_handle_of(unsigned idx)
{
    const struct ble_gatt_chr_def *c = nth_chr(idx);
    return c && c->val_handle ? *c->val_handle : 0;
}
int fake_drive_read(unsigned idx, uint16_t conn,
                    uint8_t *out, size_t cap, size_t *outlen)
{
    const struct ble_gatt_chr_def *c = nth_chr(idx);
    if (!c) return -1;
    struct os_mbuf om = { .len = 0 };
    struct ble_gatt_access_ctxt ctxt =
        { .op = BLE_GATT_ACCESS_OP_READ_CHR, .om = &om };
    int rc = c->access_cb(conn, *c->val_handle, &ctxt, c->arg);
    if (rc) return rc;
    if (om.len > cap) return -1;
    memcpy(out, om.buf, om.len);
    *outlen = om.len;
    return 0;
}
int fake_drive_write(unsigned idx, uint16_t conn,
                     const void *data, size_t len)
{
    const struct ble_gatt_chr_def *c = nth_chr(idx);
    if (!c) return -1;
    struct os_mbuf om = { .len = (uint16_t)len };
    memcpy(om.buf, data, len);
    struct ble_gatt_access_ctxt ctxt =
        { .op = BLE_GATT_ACCESS_OP_WRITE_CHR, .om = &om };
    return c->access_cb(conn, *c->val_handle, &ctxt, c->arg);
}
