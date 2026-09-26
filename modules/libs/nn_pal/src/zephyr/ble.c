/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_pal/ble.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <nn_osal/osal.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

NN_OSAL_LOG_MODULE(nn_pal_ble);

/* ============================================================
 * The PAL exposes a runtime, table-based GATT service definition.
 * Zephyr's upstream API is built around the compile-time
 * BT_GATT_SERVICE_DEFINE macro and a corresponding `struct bt_gatt_attr[]`
 * placed into an iterable section, but bt_gatt_service_register() also
 * accepts a heap-built bt_gatt_service.  This backend uses the latter:
 * we own a static pool of attribute / chrc / uuid / ccc storage and
 * fill it in at register-time.
 *
 * Capacity sized for the nn project's actual use (2 services × up to
 * 8 chrcs).  No dynamic allocation.
 * ============================================================ */

#define MAX_SVCS           2
#define MAX_CHRCS_PER_SVC  8
/* Per service: 1 primary + 2*chrcs + 1 CCC per notify chrc (cap 4 notify).
 * 1 + 16 + 4 = 21.  Round to 24. */
#define MAX_ATTRS_PER_SVC  24

#define MAX_CONN_CBS  4

/* Per-chrc adapter info — stored as attr->user_data on the VALUE attr,
 * so the read/write trampolines recover the PAL chrc pointer. */
struct chrc_slot {
    nn_pal_ble_chrc_t *pal;            /* caller-owned PAL chrc */
    struct bt_gatt_chrc zchrc;         /* Zephyr chrc declaration */
    struct bt_uuid_128  uuid;          /* parsed Zephyr UUID for chrc */
    /* Index of the VALUE attr in svc->attrs; used by notify() to find
     * the attribute pointer Zephyr wants. */
    uint16_t            value_attr_ix;
    /* Per-chrc CCC user data (only used when NOTIFY flag is set). */
    struct bt_gatt_ccc_managed_user_data ccc;
    bool                has_notify;
};

struct svc_slot {
    bool                 in_use;
    struct bt_uuid_128   svc_uuid;
    struct bt_gatt_attr  attrs[MAX_ATTRS_PER_SVC];
    size_t               attr_count;
    struct bt_gatt_service service;
    struct chrc_slot     chrcs[MAX_CHRCS_PER_SVC];
    size_t               chrc_count;
};

static struct svc_slot s_svcs[MAX_SVCS];

/* Connection callback fan-out. */
struct cb_slot {
    nn_pal_ble_conn_cb_t cb;
    void                *user;
    bool                 in_use;
};
static struct cb_slot s_conn_cbs[MAX_CONN_CBS];

/* Scan callback (only one — central use cases in nn are single-consumer). */
static nn_pal_ble_scan_cb_t s_scan_cb;
static void                *s_scan_user;

/* Connection handle ↔ pointer mapping.  We maintain our own table
 * indexed by bt_conn_index() so we can recover the bt_conn * from a
 * caller-supplied handle without iterating.  The handle is `index+1`
 * so 0 stays "invalid". */
#ifndef CONFIG_BT_MAX_CONN
#define CONFIG_BT_MAX_CONN 2
#endif
static struct bt_conn *s_conn_table[CONFIG_BT_MAX_CONN];

static struct bt_conn *handle_to_conn(nn_pal_ble_conn_t h)
{
    if (h == 0 || h > CONFIG_BT_MAX_CONN) return NULL;
    struct bt_conn *c = s_conn_table[h - 1];
    /* Don't ref here — callers that need a ref take it explicitly. */
    return c;
}

static nn_pal_ble_conn_t conn_to_handle(struct bt_conn *c)
{
    if (!c) return 0;
    return (nn_pal_ble_conn_t)(bt_conn_index(c) + 1);
}

/* ── lifecycle ────────────────────────────────────────────────── */

static bool s_inited;
static int  s_init_err;
static K_SEM_DEFINE(s_init_sem, 0, 1);

static void bt_ready_cb(int err)
{
    s_init_err = err;
    if (err == 0) {
        s_inited = true;
        NN_LOG_INF("bt enabled");
    } else {
        NN_LOG_ERR("bt enable failed: %d", err);
    }
    k_sem_give(&s_init_sem);
}

/* Block until BT host is fully up — callers (e.g. network_manager.c
 * nm_init) rely on this so they can immediately interact with BLE
 * after the call returns. */
int nn_pal_ble_init(void)
{
    if (s_inited) return 0;
    k_sem_reset(&s_init_sem);
    int rv = bt_enable(bt_ready_cb);
    /* bt_enable returns -EALREADY if already up. */
    if (rv == -EALREADY) {
        s_inited = true;
        return 0;
    }
    if (rv) return rv;
    if (k_sem_take(&s_init_sem, K_SECONDS(10)) != 0) return -ETIMEDOUT;
    return s_init_err;
}

int nn_pal_ble_shutdown(void)
{
    if (!s_inited) return 0;
    int rv = bt_disable();
    if (rv == 0) s_inited = false;
    return rv;
}

bool nn_pal_ble_is_ready(void) { return s_inited; }

int nn_pal_ble_set_device_name(const char *name)
{
    if (!name) return -EINVAL;
    return bt_set_name(name);
}

/* ── UUID parsing ─────────────────────────────────────────────── */

int nn_pal_ble_uuid_from_str(nn_pal_ble_uuid_t *out, const char *s)
{
    if (!out || !s) return -EINVAL;
    /* Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    /* Quick & local hex parser — 16 bytes, big-endian. */
    static const int positions[] = { 0, 4, 9, 14, 19, 24 };  /* unused; kept for ref */
    (void)positions;
    uint8_t bytes[16] = {0};
    int idx = 0;
    while (*s && idx < 16) {
        if (*s == '-') { s++; continue; }
        char hi = *s++;
        if (!*s) return -EINVAL;
        char lo = *s++;
        int h = (hi >= '0' && hi <= '9') ? hi - '0'
              : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
              : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
        int l = (lo >= '0' && lo <= '9') ? lo - '0'
              : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
              : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
        if (h < 0 || l < 0) return -EINVAL;
        bytes[idx++] = (uint8_t)((h << 4) | l);
    }
    if (idx != 16) return -EINVAL;
    /* BLE UUID byte order is little-endian on the wire; the PAL stores
     * "big-endian" as documented in the header, but we just pass through
     * the bytes the caller hands us — UUID equality is byte-wise either
     * way, and BT_UUID_128_ENCODE-style values are already in the right
     * order for bt_uuid_init_128. */
    memcpy(out->bytes, bytes, 16);
    return 0;
}

/* ── GATT service registration ────────────────────────────────── */

static struct svc_slot *alloc_svc(void)
{
    for (int i = 0; i < MAX_SVCS; i++) {
        if (!s_svcs[i].in_use) return &s_svcs[i];
    }
    return NULL;
}

/* The read/write adapters: Zephyr signature ↔ PAL signature. */

static ssize_t read_adapter(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset)
{
    struct chrc_slot *cs = attr->user_data;
    if (!cs || !cs->pal || !cs->pal->on_read) {
        return BT_GATT_ERR(BT_ATT_ERR_READ_NOT_PERMITTED);
    }

    /* Backend buffer the user fills in; bt_gatt_attr_read then handles
     * the offset/length chopping into the actual ATT response.  Cap at
     * 512 — the spec max for an ATT attribute, and well below the
     * largest payload the project uses (~260B dataset). */
    uint8_t tmp[512];
    size_t  outlen = 0;
    nn_pal_ble_read_ctx_t ctx = {
        .conn   = conn_to_handle(conn),
        .offset = offset,
    };
    int rv = cs->pal->on_read(&ctx, cs->pal->user, tmp, sizeof tmp, &outlen);
    if (rv == -EPERM)  return BT_GATT_ERR(BT_ATT_ERR_READ_NOT_PERMITTED);
    if (rv == -EINVAL) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    if (rv < 0)        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);

    return bt_gatt_attr_read(conn, attr, buf, len, offset, tmp, outlen);
}

static ssize_t write_adapter(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len,
                             uint16_t offset, uint8_t flags)
{
    struct chrc_slot *cs = attr->user_data;
    if (!cs || !cs->pal || !cs->pal->on_write) {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }
    uint32_t pflags = 0;
    if (flags & BT_GATT_WRITE_FLAG_PREPARE) pflags |= NN_PAL_BLE_WRITE_FLAG_PREPARE;
    if (flags & BT_GATT_WRITE_FLAG_CMD)     pflags |= NN_PAL_BLE_WRITE_FLAG_CMD;

    nn_pal_ble_write_ctx_t ctx = {
        .conn   = conn_to_handle(conn),
        .offset = offset,
        .flags  = pflags,
    };
    int rv = cs->pal->on_write(&ctx, cs->pal->user, buf, len);
    if (rv == 0)                     return (ssize_t)len;
    if (rv > 0)                      return (ssize_t)rv;   /* bytes accepted */
    if (rv == -EINVAL)               return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    if (rv == -EPERM)                return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    if (rv == -EMSGSIZE)             return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    if (rv == -ENOBUFS)              return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
}

/* CCC fan-out: the bt_gatt_ccc_managed_user_data's cfg_changed fires
 * with the new CCC value (BT_GATT_CCC_NOTIFY | BT_GATT_CCC_INDICATE).
 * We need to map back to our PAL chrc — embed a back-pointer in the
 * struct via the user_data field of the attr.  However the CCC attr
 * already uses user_data for its config struct, so we can't reuse it.
 * Workaround: walk all svc slots looking for a chrc whose embedded
 * ccc struct matches the one we got the callback for. */
static struct chrc_slot *find_chrc_by_ccc(struct bt_gatt_ccc_managed_user_data *ccc)
{
    for (int i = 0; i < MAX_SVCS; i++) {
        if (!s_svcs[i].in_use) continue;
        for (size_t j = 0; j < s_svcs[i].chrc_count; j++) {
            if (&s_svcs[i].chrcs[j].ccc == ccc) {
                return &s_svcs[i].chrcs[j];
            }
        }
    }
    return NULL;
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    /* attr->user_data is &chrc_slot::ccc. */
    struct chrc_slot *cs = find_chrc_by_ccc(attr->user_data);
    if (!cs || !cs->pal || !cs->pal->on_ccc) return;
    bool subscribed = (value & BT_GATT_CCC_NOTIFY) != 0;
    /* No conn pointer available in this hook; pass 0 — caller is
     * generally tracking the active conn from the conn callback. */
    cs->pal->on_ccc(0, cs->pal->user, subscribed);
}

/* Attribute-type UUIDs for the service table.
 *
 * These MUST be objects with static storage duration.  Zephyr's
 * BT_UUID_GATT_PRIMARY / _CHRC / _CCC macros expand to COMPOUND
 * LITERALS: at file scope (the intended BT_GATT_SERVICE_DEFINE usage)
 * those are static, but inside a function they are stack-allocated and
 * die on return.  We were storing pointers to those temporaries in the
 * long-lived attribute table — the ATT server then compared against
 * dead stack, stopped recognising the 0x2800 service declaration, and
 * folded the whole service into the preceding group's handle range
 * (GAP reported end 0x0019 on the wire), making the service
 * undiscoverable while bt_gatt_service_register() returned 0. */
static const struct bt_uuid_16 uuid_att_primary =
        BT_UUID_INIT_16(BT_UUID_GATT_PRIMARY_VAL);
static const struct bt_uuid_16 uuid_att_chrc =
        BT_UUID_INIT_16(BT_UUID_GATT_CHRC_VAL);
static const struct bt_uuid_16 uuid_att_ccc =
        BT_UUID_INIT_16(BT_UUID_GATT_CCC_VAL);

int nn_pal_ble_gatt_register_service(nn_pal_ble_service_t *svc)
{
    if (!svc || !svc->chrcs || svc->chrc_count == 0) return -EINVAL;
    if (svc->chrc_count > MAX_CHRCS_PER_SVC) return -ENOMEM;

    struct svc_slot *s = alloc_svc();
    if (!s) return -ENOMEM;
    memset(s, 0, sizeof *s);

    /* Build the service UUID.  bt_uuid_init_128 takes uint8_t[16]. */
    bt_uuid_create((struct bt_uuid *)&s->svc_uuid, svc->uuid.bytes, 16);

    /* Primary service attr. */
    size_t a = 0;
    s->attrs[a].uuid       = &uuid_att_primary.uuid;
    s->attrs[a].perm       = BT_GATT_PERM_READ;
    s->attrs[a].read       = bt_gatt_attr_read_service;
    s->attrs[a].write      = NULL;
    s->attrs[a].user_data  = &s->svc_uuid;
    a++;

    /* Per characteristic: declaration attr + value attr (+ optional CCC). */
    for (size_t i = 0; i < svc->chrc_count; i++) {
        nn_pal_ble_chrc_t *pc = &svc->chrcs[i];
        struct chrc_slot  *cs = &s->chrcs[i];
        cs->pal = pc;

        bt_uuid_create((struct bt_uuid *)&cs->uuid, pc->uuid.bytes, 16);

        uint8_t props = 0;
        uint8_t perm  = 0;
        if (pc->flags & NN_PAL_BLE_CHRC_READ) {
            props |= BT_GATT_CHRC_READ;
            perm  |= BT_GATT_PERM_READ;
        }
        if (pc->flags & NN_PAL_BLE_CHRC_WRITE) {
            props |= BT_GATT_CHRC_WRITE;
            perm  |= BT_GATT_PERM_WRITE;
        }
        if (pc->flags & NN_PAL_BLE_CHRC_NOTIFY) {
            props |= BT_GATT_CHRC_NOTIFY;
            cs->has_notify = true;
        }
        cs->zchrc.uuid         = (const struct bt_uuid *)&cs->uuid;
        cs->zchrc.properties   = props;
        /* value_handle filled in by the stack at register time. */

        /* Declaration attr. */
        s->attrs[a].uuid       = &uuid_att_chrc.uuid;
        s->attrs[a].perm       = BT_GATT_PERM_READ;
        s->attrs[a].read       = bt_gatt_attr_read_chrc;
        s->attrs[a].write      = NULL;
        s->attrs[a].user_data  = &cs->zchrc;
        a++;

        /* Value attr. */
        cs->value_attr_ix      = (uint16_t)a;
        s->attrs[a].uuid       = (const struct bt_uuid *)&cs->uuid;
        s->attrs[a].perm       = perm;
        s->attrs[a].read       = (pc->flags & NN_PAL_BLE_CHRC_READ)  ? read_adapter  : NULL;
        s->attrs[a].write      = (pc->flags & NN_PAL_BLE_CHRC_WRITE) ? write_adapter : NULL;
        s->attrs[a].user_data  = cs;
        a++;

        if (cs->has_notify) {
            /* CCC config storage (cfg array is in-struct). */
            memset(&cs->ccc, 0, sizeof cs->ccc);
            cs->ccc.cfg_changed = ccc_changed;
            s->attrs[a].uuid       = &uuid_att_ccc.uuid;
            s->attrs[a].perm       = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE;
            s->attrs[a].read       = bt_gatt_attr_read_ccc;
            s->attrs[a].write      = bt_gatt_attr_write_ccc;
            s->attrs[a].user_data  = &cs->ccc;
            a++;
        }
        if (a > MAX_ATTRS_PER_SVC) return -ENOMEM;
    }

    s->attr_count       = a;
    s->chrc_count       = svc->chrc_count;
    s->service.attrs    = s->attrs;
    s->service.attr_count = a;
    s->in_use           = true;

    int rv = bt_gatt_service_register(&s->service);
    if (rv != 0) {
        s->in_use = false;
        return rv;
    }

    /* Fill caller-visible handles: each chrc's PAL handle is the index
     * of its value attr within the service's attribute array, biased
     * by the service-instance to keep handles unique across services
     * (high 8 bits = svc index, low 16 = attr index). */
    int svc_ix = (int)(s - s_svcs);
    for (size_t i = 0; i < svc->chrc_count; i++) {
        svc->chrcs[i].handle = (uint16_t)((svc_ix << 12) |
                                          s->chrcs[i].value_attr_ix);
    }
    /* attrs[].handle now carries the real ATT handles Zephyr assigned —
     * log the range: a zero range would mean the service never entered
     * the served DB (the compound-literal-uuid bug's signature). */
    NN_LOG_INF("service[%d] registered: %u attrs (%u chrcs) "
               "att 0x%04x..0x%04x",
               svc_ix, (unsigned)s->attr_count, (unsigned)s->chrc_count,
               s->attrs[0].handle, s->attrs[s->attr_count - 1].handle);
    return 0;
}

static const struct bt_gatt_attr *lookup_attr_by_handle(uint16_t handle)
{
    int svc_ix = (handle >> 12) & 0xF;
    int aix    = handle & 0xFFF;
    if (svc_ix < 0 || svc_ix >= MAX_SVCS) return NULL;
    if (!s_svcs[svc_ix].in_use)           return NULL;
    if ((size_t)aix >= s_svcs[svc_ix].attr_count) return NULL;
    return &s_svcs[svc_ix].attrs[aix];
}

int nn_pal_ble_gatt_notify(nn_pal_ble_conn_t conn, uint16_t chrc_handle,
                           const void *data, size_t len)
{
    const struct bt_gatt_attr *attr = lookup_attr_by_handle(chrc_handle);
    if (!attr) return -EINVAL;
    /* conn=0 → broadcast to all subscribed centrals (Zephyr behavior). */
    struct bt_conn *bc = handle_to_conn(conn);
    return bt_gatt_notify(bc, attr, data, len);
}

/* ── advertising ──────────────────────────────────────────────── */

static uint8_t s_adv_uuid_bytes[16];      /* keep alive for adv */
static struct bt_data s_ad_storage[3];
static struct bt_data s_sd_storage[1];

int nn_pal_ble_advertise_start(const nn_pal_ble_adv_params_t *p)
{
    struct bt_le_adv_param ap = {
        .options       = (p && p->connectable) ? BT_LE_ADV_OPT_CONN
                                                : BT_LE_ADV_OPT_NONE,
        .interval_min  = (p && p->interval_ms_min) ?
                         (uint16_t)((p->interval_ms_min * 8) / 5) :
                         BT_GAP_ADV_FAST_INT_MIN_2,
        .interval_max  = (p && p->interval_ms_max) ?
                         (uint16_t)((p->interval_ms_max * 8) / 5) :
                         BT_GAP_ADV_FAST_INT_MAX_2,
        .peer          = NULL,
    };
    /* Default to connectable if no params struct provided at all. */
    if (!p) ap.options = BT_LE_ADV_OPT_CONN;

    size_t ad_count = 0;
    uint8_t flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
    s_ad_storage[ad_count].type     = BT_DATA_FLAGS;
    s_ad_storage[ad_count].data     = &flags;
    s_ad_storage[ad_count].data_len = 1;
    ad_count++;

    if (p && p->service_uuid) {
        memcpy(s_adv_uuid_bytes, p->service_uuid->bytes, 16);
        s_ad_storage[ad_count].type     = BT_DATA_UUID128_ALL;
        s_ad_storage[ad_count].data     = s_adv_uuid_bytes;
        s_ad_storage[ad_count].data_len = 16;
        ad_count++;
    }

    /* Device name in scan response. */
    const char *name = bt_get_name();
    s_sd_storage[0].type     = BT_DATA_NAME_COMPLETE;
    s_sd_storage[0].data     = (const uint8_t *)name;
    s_sd_storage[0].data_len = name ? (uint8_t)strlen(name) : 0;

    return bt_le_adv_start(&ap, s_ad_storage, ad_count, s_sd_storage, 1);
}

int nn_pal_ble_advertise_stop(void)
{
    return bt_le_adv_stop();
}

/* ── scan ─────────────────────────────────────────────────────── */

/* AD-parse helper: extract 128-bit service UUIDs from an advertisement
 * payload.  Caller passes in a buffer to fill; we return the count. */
#define SCAN_UUID_CAP  4
struct ad_parse_ctx {
    nn_pal_ble_uuid_t uuids[SCAN_UUID_CAP];
    size_t            count;
};

static bool parse_ad_uuid128(struct bt_data *data, void *user_data)
{
    struct ad_parse_ctx *ctx = user_data;
    if (data->type != BT_DATA_UUID128_ALL &&
        data->type != BT_DATA_UUID128_SOME) {
        return true;   /* keep scanning */
    }
    if (data->data_len % 16 != 0) return true;
    for (size_t off = 0; off + 16 <= data->data_len; off += 16) {
        if (ctx->count >= SCAN_UUID_CAP) return false;
        memcpy(ctx->uuids[ctx->count].bytes, &data->data[off], 16);
        ctx->count++;
    }
    return true;
}

static void scan_recv_cb(const struct bt_le_scan_recv_info *info,
                         struct net_buf_simple *buf)
{
    if (!s_scan_cb) return;

    struct ad_parse_ctx pc = { .count = 0 };
    if (buf) {
        /* bt_data_parse consumes the buffer; clone the read offset to
         * keep `buf` reusable for the next scan event. */
        struct net_buf_simple copy = *buf;
        bt_data_parse(&copy, parse_ad_uuid128, &pc);
    }

    nn_pal_ble_scan_result_t r = {
        .rssi_dbm       = info->rssi,
        .addr_type      = info->addr ? info->addr->type : 0,
        .adv_uuids      = pc.count ? pc.uuids : NULL,
        .adv_uuid_count = pc.count,
    };
    if (info->addr) memcpy(r.addr, info->addr->a.val, 6);
    s_scan_cb(&r, s_scan_user);
}

static struct bt_le_scan_cb s_scan_cbs = { .recv = scan_recv_cb };
static bool s_scan_registered;

int nn_pal_ble_scan_start(const nn_pal_ble_scan_params_t *params,
                          nn_pal_ble_scan_cb_t cb, void *user)
{
    s_scan_cb   = cb;
    s_scan_user = user;
    if (!s_scan_registered) {
        bt_le_scan_cb_register(&s_scan_cbs);
        s_scan_registered = true;
    }
    bool passive = !params || params->passive;
    struct bt_le_scan_param p = {
        .type     = passive ? BT_LE_SCAN_TYPE_PASSIVE
                            : BT_LE_SCAN_TYPE_ACTIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window   = BT_GAP_SCAN_FAST_WINDOW,
    };
    return bt_le_scan_start(&p, NULL);
}

int nn_pal_ble_scan_stop(void)
{
    return bt_le_scan_stop();
}

/* ── connection callbacks ─────────────────────────────────────── */

static void connected_trampoline(struct bt_conn *conn, uint8_t err)
{
    uint8_t ix = bt_conn_index(conn);
    if (ix < CONFIG_BT_MAX_CONN && !err) {
        s_conn_table[ix] = conn;   /* aliased; backend doesn't ref */
    }
    nn_pal_ble_conn_info_t info = { .mtu = bt_gatt_get_mtu(conn) };
    const bt_addr_le_t *p = bt_conn_get_dst(conn);
    if (p) {
        memcpy(info.peer_addr, p->a.val, 6);
        info.peer_addr_type = p->type;
    }
    nn_pal_ble_conn_t h = conn_to_handle(conn);
    for (int i = 0; i < MAX_CONN_CBS; i++) {
        if (s_conn_cbs[i].in_use && s_conn_cbs[i].cb) {
            s_conn_cbs[i].cb(h, err ? NN_PAL_BLE_CONN_EV_DISCONNECTED
                                     : NN_PAL_BLE_CONN_EV_CONNECTED,
                              &info, s_conn_cbs[i].user);
        }
    }
}

static void disconnected_trampoline(struct bt_conn *conn, uint8_t reason)
{
    nn_pal_ble_conn_info_t info = {
        .mtu               = 0,
        .disconnect_reason = reason,
    };
    nn_pal_ble_conn_t h = conn_to_handle(conn);
    for (int i = 0; i < MAX_CONN_CBS; i++) {
        if (s_conn_cbs[i].in_use && s_conn_cbs[i].cb) {
            s_conn_cbs[i].cb(h, NN_PAL_BLE_CONN_EV_DISCONNECTED,
                              &info, s_conn_cbs[i].user);
        }
    }
    uint8_t ix = bt_conn_index(conn);
    if (ix < CONFIG_BT_MAX_CONN) {
        s_conn_table[ix] = NULL;
    }
}

static struct bt_conn_cb s_bt_conn_cb = {
    .connected    = connected_trampoline,
    .disconnected = disconnected_trampoline,
};

static bool s_conn_cb_registered;

int nn_pal_ble_conn_cb_register(nn_pal_ble_conn_cb_t cb, void *user)
{
    if (!cb) return -EINVAL;
    if (!s_conn_cb_registered) {
        bt_conn_cb_register(&s_bt_conn_cb);
        s_conn_cb_registered = true;
    }
    for (int i = 0; i < MAX_CONN_CBS; i++) {
        if (!s_conn_cbs[i].in_use) {
            s_conn_cbs[i].cb     = cb;
            s_conn_cbs[i].user   = user;
            s_conn_cbs[i].in_use = true;
            return 0;
        }
    }
    return -ENOMEM;
}

int nn_pal_ble_conn_disconnect(nn_pal_ble_conn_t conn)
{
    struct bt_conn *c = handle_to_conn(conn);
    if (!c) return -ENOENT;
    /* Note: c is the aliased pointer from s_conn_table — don't unref. */
    return bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

int nn_pal_ble_connect(const uint8_t addr[6], uint8_t addr_type)
{
    if (!addr) return -EINVAL;
    bt_addr_le_t a = { .type = addr_type };
    memcpy(a.a.val, addr, 6);
    struct bt_conn *c = NULL;
    int rv = bt_conn_le_create(&a, BT_CONN_LE_CREATE_CONN,
                               BT_LE_CONN_PARAM_DEFAULT, &c);
    if (c) bt_conn_unref(c);   /* connected_trampoline will see it */
    return rv;
}

/* ── GATT client ─────────────────────────────────────────────── */

/* Caller-driven (sync via sem) flows want a backend that owns the
 * Zephyr request structs (gatt_exchange_params, gatt_discover_params,
 * gatt_read_params, gatt_write_params, gatt_subscribe_params) for the
 * lifetime of each operation.  Provisioning is single-threaded, so a
 * single in-flight slot per op type is sufficient. */

/* ── MTU exchange ───────────────────────────────────────────── */

static K_SEM_DEFINE(s_mtu_sem, 0, 1);
static int  s_mtu_err;

static void mtu_done(struct bt_conn *conn, uint8_t err,
                     struct bt_gatt_exchange_params *params)
{
    ARG_UNUSED(conn); ARG_UNUSED(params);
    s_mtu_err = err;
    k_sem_give(&s_mtu_sem);
}

int nn_pal_ble_gatt_mtu_exchange(nn_pal_ble_conn_t conn)
{
    struct bt_conn *c = handle_to_conn(conn);
    if (!c) return -ENOTCONN;
    static struct bt_gatt_exchange_params p;
    p.func = mtu_done;
    k_sem_reset(&s_mtu_sem);
    int rv = bt_gatt_exchange_mtu(c, &p);
    if (rv == -EALREADY) return 0;
    if (rv) return rv;
    if (k_sem_take(&s_mtu_sem, K_SECONDS(5)) != 0) return -ETIMEDOUT;
    return s_mtu_err ? -EIO : 0;
}

/* ── discovery state machine ────────────────────────────────── */

enum disco_phase {
    DISCO_PRIMARY,
    DISCO_CHRCS,
    DISCO_CCC,
};

struct disco_state {
    enum disco_phase           phase;
    struct bt_gatt_discover_params params;
    struct bt_uuid_128         svc_uuid;
    uint16_t                   svc_start;
    uint16_t                   svc_end;
    nn_pal_ble_remote_chrc_t  *chrcs;
    size_t                     chrc_count;
    /* For phase DISCO_CCC: index into chrcs[] of the chrc whose CCC
     * we're currently looking for. */
    size_t                     ccc_ix;
    nn_pal_ble_disco_cb_t      cb;
    void                      *user;
    nn_pal_ble_conn_t          conn;
    bool                       in_use;
};

static struct disco_state s_disco;

/* Find the next chrc (after start) that has a non-zero value_handle
 * (meaning we found it during DISCO_CHRCS) so we know to look for its
 * CCC.  Returns chrc_count if none remain. */
static size_t next_ccc_target(size_t start)
{
    for (size_t i = start; i < s_disco.chrc_count; i++) {
        if (s_disco.chrcs[i].value_handle != 0) return i;
    }
    return s_disco.chrc_count;
}

static void disco_finish(int err)
{
    nn_pal_ble_disco_cb_t cb = s_disco.cb;
    void *user               = s_disco.user;
    nn_pal_ble_conn_t conn   = s_disco.conn;
    s_disco.in_use = false;
    if (cb) cb(conn, err, user);
}

static uint8_t disco_cb(struct bt_conn *bc,
                        const struct bt_gatt_attr *attr,
                        struct bt_gatt_discover_params *params);

static int kick_ccc_phase(struct bt_conn *bc, size_t ix)
{
    s_disco.phase   = DISCO_CCC;
    s_disco.ccc_ix  = ix;
    /* CCC sits in the descriptor range immediately after the value
     * handle; cap at the next chrc's handle (or svc_end). */
    uint16_t next_handle = s_disco.svc_end;
    for (size_t j = 0; j < s_disco.chrc_count; j++) {
        uint16_t vh = s_disco.chrcs[j].value_handle;
        if (vh > s_disco.chrcs[ix].value_handle && vh < next_handle) {
            next_handle = vh;
        }
    }
    /* Static object, not the compound-literal macro: the discover
     * procedure holds this pointer long after this function returns. */
    s_disco.params.uuid         = &uuid_att_ccc.uuid;
    s_disco.params.start_handle = s_disco.chrcs[ix].value_handle + 1;
    s_disco.params.end_handle   = next_handle;
    s_disco.params.type         = BT_GATT_DISCOVER_DESCRIPTOR;
    s_disco.params.func         = disco_cb;
    return bt_gatt_discover(bc, &s_disco.params);
}

static uint8_t disco_cb(struct bt_conn *bc,
                        const struct bt_gatt_attr *attr,
                        struct bt_gatt_discover_params *params)
{
    if (!attr) {
        /* End of current phase.  Advance. */
        if (s_disco.phase == DISCO_PRIMARY) {
            /* Primary phase ended without a hit. */
            disco_finish(-ENOENT);
            return BT_GATT_ITER_STOP;
        }
        if (s_disco.phase == DISCO_CHRCS) {
            /* Move to CCC phase for the first chrc that needs one. */
            size_t next = next_ccc_target(0);
            if (next == s_disco.chrc_count) {
                disco_finish(0);
                return BT_GATT_ITER_STOP;
            }
            int rv = kick_ccc_phase(bc, next);
            if (rv) disco_finish(rv);
            return BT_GATT_ITER_STOP;
        }
        if (s_disco.phase == DISCO_CCC) {
            /* Either we found a CCC already (handled below) or this
             * chrc has no CCC — advance to the next ccc target. */
            size_t next = next_ccc_target(s_disco.ccc_ix + 1);
            if (next == s_disco.chrc_count) {
                disco_finish(0);
                return BT_GATT_ITER_STOP;
            }
            int rv = kick_ccc_phase(bc, next);
            if (rv) disco_finish(rv);
            return BT_GATT_ITER_STOP;
        }
    }

    if (s_disco.phase == DISCO_PRIMARY) {
        const struct bt_gatt_service_val *svc = attr->user_data;
        s_disco.svc_start = attr->handle;
        s_disco.svc_end   = svc->end_handle;
        /* Kick chrc enumeration over (svc_start..svc_end]. */
        s_disco.phase   = DISCO_CHRCS;
        s_disco.params.uuid         = NULL;
        s_disco.params.start_handle = s_disco.svc_start + 1;
        s_disco.params.end_handle   = s_disco.svc_end;
        s_disco.params.type         = BT_GATT_DISCOVER_CHARACTERISTIC;
        bt_gatt_discover(bc, &s_disco.params);
        return BT_GATT_ITER_STOP;
    }

    if (s_disco.phase == DISCO_CHRCS) {
        const struct bt_gatt_chrc *chrc = attr->user_data;
        /* chrc->uuid points to a bt_uuid_128 the stack synthesised.
         * Compare its bytes against each of our targets. */
        if (chrc && chrc->uuid && chrc->uuid->type == BT_UUID_TYPE_128) {
            const struct bt_uuid_128 *u = (const void *)chrc->uuid;
            for (size_t i = 0; i < s_disco.chrc_count; i++) {
                if (memcmp(u->val, s_disco.chrcs[i].uuid.bytes, 16) == 0) {
                    s_disco.chrcs[i].value_handle = chrc->value_handle;
                    break;
                }
            }
        }
        return BT_GATT_ITER_CONTINUE;
    }

    if (s_disco.phase == DISCO_CCC) {
        /* Match: this attr's handle is the CCC for the in-flight chrc. */
        s_disco.chrcs[s_disco.ccc_ix].ccc_handle = attr->handle;
        /* Continue to next target. */
        size_t next = next_ccc_target(s_disco.ccc_ix + 1);
        if (next == s_disco.chrc_count) {
            disco_finish(0);
            return BT_GATT_ITER_STOP;
        }
        int rv = kick_ccc_phase(bc, next);
        if (rv) disco_finish(rv);
        return BT_GATT_ITER_STOP;
    }
    return BT_GATT_ITER_CONTINUE;
}

int nn_pal_ble_gatt_discover(nn_pal_ble_conn_t conn,
                             const nn_pal_ble_uuid_t *svc_uuid,
                             nn_pal_ble_remote_chrc_t *chrcs,
                             size_t chrc_count,
                             nn_pal_ble_disco_cb_t cb, void *user)
{
    if (!svc_uuid || !chrcs || !chrc_count || !cb) return -EINVAL;
    if (s_disco.in_use) return -EBUSY;
    struct bt_conn *c = handle_to_conn(conn);
    if (!c) return -ENOTCONN;

    /* Zero-init the caller's handle fields so partial discoveries
     * leave them at 0. */
    for (size_t i = 0; i < chrc_count; i++) {
        chrcs[i].value_handle = 0;
        chrcs[i].ccc_handle   = 0;
    }

    s_disco.in_use     = true;
    s_disco.conn       = conn;
    s_disco.chrcs      = chrcs;
    s_disco.chrc_count = chrc_count;
    s_disco.cb         = cb;
    s_disco.user       = user;
    s_disco.phase      = DISCO_PRIMARY;
    bt_uuid_create((struct bt_uuid *)&s_disco.svc_uuid, svc_uuid->bytes, 16);
    s_disco.params.uuid         = (const struct bt_uuid *)&s_disco.svc_uuid;
    s_disco.params.func         = disco_cb;
    s_disco.params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    s_disco.params.end_handle   = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    s_disco.params.type         = BT_GATT_DISCOVER_PRIMARY;
    int rv = bt_gatt_discover(c, &s_disco.params);
    if (rv) {
        s_disco.in_use = false;
        return rv;
    }
    return 0;
}

/* ── read ──────────────────────────────────────────────────── */

static struct {
    bool                       in_use;
    struct bt_gatt_read_params params;
    nn_pal_ble_gatt_read_cb_t  cb;
    void                      *user;
    nn_pal_ble_conn_t          conn;
    uint8_t                    buf[512];
    size_t                     len;
} s_read;

static uint8_t read_done(struct bt_conn *bc, uint8_t err,
                         struct bt_gatt_read_params *params,
                         const void *data, uint16_t length)
{
    ARG_UNUSED(bc); ARG_UNUSED(params);
    if (err) {
        s_read.in_use = false;
        if (s_read.cb) s_read.cb(s_read.conn, -EIO, NULL, 0, s_read.user);
        return BT_GATT_ITER_STOP;
    }
    if (!data) {
        nn_pal_ble_gatt_read_cb_t cb = s_read.cb;
        nn_pal_ble_conn_t conn       = s_read.conn;
        const uint8_t *buf           = s_read.buf;
        size_t len                   = s_read.len;
        void *user                   = s_read.user;
        s_read.in_use = false;
        if (cb) cb(conn, 0, buf, len, user);
        return BT_GATT_ITER_STOP;
    }
    size_t cap = sizeof s_read.buf - s_read.len;
    size_t cpy = length < cap ? length : cap;
    memcpy(s_read.buf + s_read.len, data, cpy);
    s_read.len += cpy;
    return BT_GATT_ITER_CONTINUE;
}

int nn_pal_ble_gatt_read(nn_pal_ble_conn_t conn, uint16_t handle,
                         nn_pal_ble_gatt_read_cb_t cb, void *user)
{
    if (!cb) return -EINVAL;
    if (s_read.in_use) return -EBUSY;
    struct bt_conn *c = handle_to_conn(conn);
    if (!c) return -ENOTCONN;
    s_read.in_use = true;
    s_read.cb     = cb;
    s_read.user   = user;
    s_read.conn   = conn;
    s_read.len    = 0;
    s_read.params.func           = read_done;
    s_read.params.handle_count   = 1;
    s_read.params.single.handle  = handle;
    s_read.params.single.offset  = 0;
    int rv = bt_gatt_read(c, &s_read.params);
    if (rv) s_read.in_use = false;
    return rv;
}

/* ── write ─────────────────────────────────────────────────── */

static struct {
    bool                        in_use;
    struct bt_gatt_write_params params;
    nn_pal_ble_gatt_write_cb_t  cb;
    void                       *user;
    nn_pal_ble_conn_t           conn;
} s_write;

static void write_done(struct bt_conn *bc, uint8_t err,
                       struct bt_gatt_write_params *params)
{
    ARG_UNUSED(bc); ARG_UNUSED(params);
    nn_pal_ble_gatt_write_cb_t cb = s_write.cb;
    nn_pal_ble_conn_t conn        = s_write.conn;
    void *user                    = s_write.user;
    s_write.in_use = false;
    if (cb) cb(conn, err ? -EIO : 0, user);
}

int nn_pal_ble_gatt_write(nn_pal_ble_conn_t conn, uint16_t handle,
                          const void *data, size_t len,
                          nn_pal_ble_gatt_write_cb_t cb, void *user)
{
    if (!data || !cb) return -EINVAL;
    if (s_write.in_use) return -EBUSY;
    struct bt_conn *c = handle_to_conn(conn);
    if (!c) return -ENOTCONN;
    s_write.in_use = true;
    s_write.cb     = cb;
    s_write.user   = user;
    s_write.conn   = conn;
    s_write.params.func   = write_done;
    s_write.params.handle = handle;
    s_write.params.offset = 0;
    s_write.params.data   = data;
    s_write.params.length = (uint16_t)len;
    int rv = bt_gatt_write(c, &s_write.params);
    if (rv) s_write.in_use = false;
    return rv;
}

/* ── subscribe ─────────────────────────────────────────────── */

#define MAX_SUBS 4
struct sub_slot {
    bool                            in_use;
    struct bt_gatt_subscribe_params params;
    nn_pal_ble_gatt_notify_cb_t     cb;
    void                           *user;
    nn_pal_ble_conn_t               conn;
};
static struct sub_slot s_subs[MAX_SUBS];

static uint8_t notify_trampoline(struct bt_conn *bc,
                                 struct bt_gatt_subscribe_params *params,
                                 const void *data, uint16_t length)
{
    ARG_UNUSED(bc);
    /* params points to the embedded params field of one of our slots —
     * recover via CONTAINER_OF. */
    struct sub_slot *s = CONTAINER_OF(params, struct sub_slot, params);
    if (!data) {
        /* Server-initiated unsubscribe — clear our slot. */
        s->in_use = false;
        return BT_GATT_ITER_STOP;
    }
    if (!s->cb || !s->cb(s->conn, data, length, s->user)) {
        s->in_use = false;
        return BT_GATT_ITER_STOP;
    }
    return BT_GATT_ITER_CONTINUE;
}

int nn_pal_ble_gatt_subscribe(nn_pal_ble_conn_t conn,
                              uint16_t value_handle, uint16_t ccc_handle,
                              nn_pal_ble_gatt_notify_cb_t cb, void *user)
{
    if (!cb || !value_handle || !ccc_handle) return -EINVAL;
    struct bt_conn *c = handle_to_conn(conn);
    if (!c) return -ENOTCONN;
    struct sub_slot *s = NULL;
    for (int i = 0; i < MAX_SUBS; i++) {
        if (!s_subs[i].in_use) { s = &s_subs[i]; break; }
    }
    if (!s) return -ENOMEM;
    s->in_use = true;
    s->cb     = cb;
    s->user   = user;
    s->conn   = conn;
    s->params.notify       = notify_trampoline;
    s->params.value_handle = value_handle;
    s->params.ccc_handle   = ccc_handle;
    s->params.value        = BT_GATT_CCC_NOTIFY;
    int rv = bt_gatt_subscribe(c, &s->params);
    if (rv && rv != -EALREADY) {
        s->in_use = false;
        return rv;
    }
    return 0;
}

/* ── pairing ──────────────────────────────────────────────────── */

int nn_pal_ble_set_pairing_mode(nn_pal_ble_pairing_mode_t m)
{
    /* Currently only JUST_WORKS is wired — that's what NoInputNoOutput
     * I/O capability negotiates by default.  Passkey modes need a
     * bt_conn_auth_cb registration; expose when needed. */
    if (m != NN_PAL_BLE_PAIRING_JUST_WORKS) return -ENOTSUP;
    return 0;
}
