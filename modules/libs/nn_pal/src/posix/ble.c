/* SPDX-License-Identifier: Apache-2.0 */
/* PTHREAD_MUTEX_RECURSIVE is XSI, and the host test suite builds with
 * -std=c11, which sets __STRICT_ANSI__ and hides it.  This backend is
 * Linux/sd-bus only, so say so rather than fight the feature macros. */
#define _GNU_SOURCE
/*
 * nn_pal BLE — POSIX / BlueZ backend (peripheral role).
 *
 * Exports a GATT application over D-Bus and hands it to bluetoothd, so a
 * Linux camera can present the SAME provisioning service an ESP camera
 * does.  That is the whole point of doing this at the PAL layer rather
 * than writing a camera-specific BlueZ daemon: nn_prov_ble.c compiles
 * unchanged, so the device speaks the byte-identical GATT contract the
 * hub's provisioning wizard already knows, and the wizard needs no
 * Linux-specific branch.
 *
 * Objects exported (mirroring BlueZ's expectations):
 *   /nn/pal/app                 org.freedesktop.DBus.ObjectManager
 *   /nn/pal/app/svc0            org.bluez.GattService1
 *   /nn/pal/app/svc0/chrcN      org.bluez.GattCharacteristic1
 *   /nn/pal/adv0                org.bluez.LEAdvertisement1
 * then RegisterApplication on org.bluez.GattManager1 and
 * RegisterAdvertisement on org.bluez.LEAdvertisingManager1.
 *
 * CENTRAL-role calls return -ENOTSUP.  Setup mode only needs peripheral,
 * and on this hardware the central path is a known-broken controller
 * feature anyway (see project memory: cc33xx central-role LL defect).
 *
 * Threading: one backend thread runs the sd-bus loop and every callback
 * fires from it, which is the contract nn_pal/ble.h promises callers.
 */

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#include <nn_pal/ble.h>
#include <nn_osal/log.h>

NN_OSAL_LOG_MODULE(nn_pal_ble);

#define APP_PATH   "/nn/pal/app"
#define SVC_PATH   APP_PATH "/svc0"
#define ADV_PATH   "/nn/pal/adv0"
#define MAX_CHRCS  8
#define MAX_CONNS  4

struct chrc_slot {
    nn_pal_ble_chrc_t *def;          /* caller-owned, outlives us */
    char               path[96];
    char               uuid_str[40];
    bool               notifying;
    uint8_t            last_value[512];
    size_t             last_len;
};

struct conn_slot {
    char              device[160];   /* BlueZ device object path */
    nn_pal_ble_conn_t id;            /* 1-based; 0 means free */
};

static sd_bus            *g_bus;
static pthread_t          g_thread;
static atomic_bool        g_run;
static atomic_bool        g_ready;
static char               g_adapter[128] = "/org/bluez/hci0";
static char               g_name[64]     = "nn-camera";
static char               g_svc_uuid[40];
static struct chrc_slot   g_chrcs[MAX_CHRCS];
static size_t             g_chrc_count;
static struct conn_slot   g_conns[MAX_CONNS];
static nn_pal_ble_conn_cb_t g_conn_cb;
static void              *g_conn_user;
static nn_pal_ble_adv_params_t g_adv;
static bool               g_adv_active;   /* BlueZ holds our advertisement */
static bool               g_adv_wanted;   /* the app asked to advertise */
static pthread_mutex_t    g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── UUID helpers ─────────────────────────────────────────────────── */

int nn_pal_ble_uuid_from_str(nn_pal_ble_uuid_t *out, const char *s)
{
    if (!out || !s) return -EINVAL;
    unsigned b[16];
    if (sscanf(s, "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7],
               &b[8], &b[9], &b[10], &b[11], &b[12], &b[13], &b[14],
               &b[15]) != 16)
        return -EINVAL;
    for (int i = 0; i < 16; i++) out->bytes[i] = (uint8_t)b[i];
    return 0;
}

static void uuid_to_str(const nn_pal_ble_uuid_t *u, char *out, size_t cap)
{
    const uint8_t *b = u->bytes;
    snprintf(out, cap,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

/* ── connection bookkeeping ───────────────────────────────────────
 * BlueZ identifies the peer by device object path in the ReadValue /
 * WriteValue options dict.  We map that to a small 1-based id because
 * the PAL promises a uint16 handle and reserves 0 for "invalid" — a
 * zero-based scheme once cost this project a whole provisioning
 * outage on the ESP side, so do not start at 0.
 */
static nn_pal_ble_conn_t conn_for_device(const char *device)
{
    if (!device || !*device) return 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_CONNS; i++)
        if (g_conns[i].id && !strcmp(g_conns[i].device, device)) {
            nn_pal_ble_conn_t id = g_conns[i].id;
            pthread_mutex_unlock(&g_lock);
            return id;
        }
    for (int i = 0; i < MAX_CONNS; i++)
        if (!g_conns[i].id) {
            snprintf(g_conns[i].device, sizeof g_conns[i].device, "%s", device);
            g_conns[i].id = (nn_pal_ble_conn_t)(i + 1);
            nn_pal_ble_conn_cb_t cb = g_conn_cb;
            void *user = g_conn_user;
            nn_pal_ble_conn_t id = g_conns[i].id;
            pthread_mutex_unlock(&g_lock);
            if (cb) {
                nn_pal_ble_conn_info_t info = {0};
                cb(id, NN_PAL_BLE_CONN_EV_CONNECTED, &info, user);
            }
            return id;
        }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static const char *device_from_options(sd_bus_message *m)
{
    /* options is a{sv}; we only care about "device". */
    static __thread char dev[160];
    dev[0] = '\0';
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return dev;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char *key = NULL;
        if (sd_bus_message_read_basic(m, 's', &key) < 0) break;
        if (key && !strcmp(key, "device")) {
            const char *v = NULL;
            if (sd_bus_message_enter_container(m, 'v', "o") > 0) {
                if (sd_bus_message_read_basic(m, 'o', &v) >= 0 && v)
                    snprintf(dev, sizeof dev, "%s", v);
                sd_bus_message_exit_container(m);
            } else {
                sd_bus_message_skip(m, "v");
            }
        } else {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    return dev;
}

static struct chrc_slot *slot_for_path(const char *path)
{
    for (size_t i = 0; i < g_chrc_count; i++)
        if (!strcmp(g_chrcs[i].path, path)) return &g_chrcs[i];
    return NULL;
}

/* ── GattService1 ─────────────────────────────────────────────────── */

static int svc_get_uuid(sd_bus *bus, const char *path, const char *iface,
                        const char *prop, sd_bus_message *reply,
                        void *userdata, sd_bus_error *err)
{
    (void)bus; (void)path; (void)iface; (void)prop; (void)userdata; (void)err;
    return sd_bus_message_append(reply, "s", g_svc_uuid);
}

static int svc_get_primary(sd_bus *bus, const char *path, const char *iface,
                           const char *prop, sd_bus_message *reply,
                           void *userdata, sd_bus_error *err)
{
    (void)bus; (void)path; (void)iface; (void)prop; (void)userdata; (void)err;
    return sd_bus_message_append(reply, "b", 1);
}

static const sd_bus_vtable svc_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID",    "s", svc_get_uuid,    0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Primary", "b", svc_get_primary, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

/* ── GattCharacteristic1 ──────────────────────────────────────────── */

static int chrc_get_uuid(sd_bus *bus, const char *path, const char *iface,
                         const char *prop, sd_bus_message *reply,
                         void *userdata, sd_bus_error *err)
{
    (void)bus; (void)iface; (void)prop; (void)userdata; (void)err;
    struct chrc_slot *s = slot_for_path(path);
    return sd_bus_message_append(reply, "s", s ? s->uuid_str : "");
}

static int chrc_get_service(sd_bus *bus, const char *path, const char *iface,
                            const char *prop, sd_bus_message *reply,
                            void *userdata, sd_bus_error *err)
{
    (void)bus; (void)path; (void)iface; (void)prop; (void)userdata; (void)err;
    return sd_bus_message_append(reply, "o", SVC_PATH);
}

static int chrc_get_flags(sd_bus *bus, const char *path, const char *iface,
                          const char *prop, sd_bus_message *reply,
                          void *userdata, sd_bus_error *err)
{
    (void)bus; (void)iface; (void)prop; (void)userdata; (void)err;
    struct chrc_slot *s = slot_for_path(path);
    uint32_t f = s && s->def ? s->def->flags : 0;
    int rc = sd_bus_message_open_container(reply, 'a', "s");
    if (rc < 0) return rc;
    if (f & NN_PAL_BLE_CHRC_READ)     sd_bus_message_append(reply, "s", "read");
    if (f & NN_PAL_BLE_CHRC_WRITE)    sd_bus_message_append(reply, "s", "write");
    if (f & NN_PAL_BLE_CHRC_NOTIFY)   sd_bus_message_append(reply, "s", "notify");
    if (f & NN_PAL_BLE_CHRC_INDICATE) sd_bus_message_append(reply, "s", "indicate");
    return sd_bus_message_close_container(reply);
}

static int chrc_get_value(sd_bus *bus, const char *path, const char *iface,
                          const char *prop, sd_bus_message *reply,
                          void *userdata, sd_bus_error *err)
{
    (void)bus; (void)iface; (void)prop; (void)userdata; (void)err;
    struct chrc_slot *s = slot_for_path(path);
    return sd_bus_message_append_array(reply, 'y',
                                       s ? s->last_value : NULL,
                                       s ? s->last_len : 0);
}

static int chrc_read(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata;
    struct chrc_slot *s = slot_for_path(sd_bus_message_get_path(m));
    if (!s || !s->def || !s->def->on_read)
        return sd_bus_error_set_const(err, "org.bluez.Error.NotSupported", "no reader");

    const char *device = device_from_options(m);
    nn_pal_ble_read_ctx_t ctx = { .conn = conn_for_device(device), .offset = 0 };

    uint8_t buf[512];
    size_t len = 0;
    int rc = s->def->on_read(&ctx, s->def->user, buf, sizeof buf, &len);
    if (rc < 0)
        return sd_bus_error_set_const(err, "org.bluez.Error.NotPermitted",
                                      "read rejected");
    sd_bus_message *reply = NULL;
    rc = sd_bus_message_new_method_return(m, &reply);
    if (rc < 0) return rc;
    rc = sd_bus_message_append_array(reply, 'y', buf, len);
    if (rc < 0) { sd_bus_message_unref(reply); return rc; }
    rc = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    return rc < 0 ? rc : 1;
}

static int chrc_write(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata;
    struct chrc_slot *s = slot_for_path(sd_bus_message_get_path(m));
    if (!s || !s->def || !s->def->on_write)
        return sd_bus_error_set_const(err, "org.bluez.Error.NotSupported", "no writer");

    const void *data = NULL;
    size_t len = 0;
    int rc = sd_bus_message_read_array(m, 'y', &data, &len);
    if (rc < 0) return rc;

    const char *device = device_from_options(m);
    nn_pal_ble_write_ctx_t ctx = { .conn = conn_for_device(device),
                                   .offset = 0, .flags = 0 };
    rc = s->def->on_write(&ctx, s->def->user, (const uint8_t *)data, len);
    if (rc < 0)
        return sd_bus_error_set_const(err, "org.bluez.Error.NotPermitted",
                                      "write rejected");
    return sd_bus_reply_method_return(m, NULL);
}

static int chrc_start_notify(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata; (void)err;
    struct chrc_slot *s = slot_for_path(sd_bus_message_get_path(m));
    if (s) {
        s->notifying = true;
        if (s->def && s->def->on_ccc)
            s->def->on_ccc(conn_for_device(device_from_options(m)),
                           s->def->user, true);
    }
    return sd_bus_reply_method_return(m, NULL);
}

static int chrc_stop_notify(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata; (void)err;
    struct chrc_slot *s = slot_for_path(sd_bus_message_get_path(m));
    if (s) {
        s->notifying = false;
        if (s->def && s->def->on_ccc)
            s->def->on_ccc(0, s->def->user, false);
    }
    return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable chrc_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID",    "s",  chrc_get_uuid,    0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service", "o",  chrc_get_service, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags",   "as", chrc_get_flags,   0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Value",   "ay", chrc_get_value,   0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD_WITH_ARGS("ReadValue",
        SD_BUS_ARGS("a{sv}", options), SD_BUS_RESULT("ay", value), chrc_read, 0),
    SD_BUS_METHOD_WITH_ARGS("WriteValue",
        SD_BUS_ARGS("ay", value, "a{sv}", options), SD_BUS_NO_RESULT, chrc_write, 0),
    SD_BUS_METHOD("StartNotify", NULL, NULL, chrc_start_notify, 0),
    SD_BUS_METHOD("StopNotify",  NULL, NULL, chrc_stop_notify,  0),
    SD_BUS_VTABLE_END
};

/* ── LEAdvertisement1 ─────────────────────────────────────────────── */

static int adv_get_type(sd_bus *bus, const char *p, const char *i,
                        const char *pr, sd_bus_message *reply, void *u,
                        sd_bus_error *e)
{
    (void)bus; (void)p; (void)i; (void)pr; (void)u; (void)e;
    return sd_bus_message_append(reply, "s", "peripheral");
}

static int adv_get_uuids(sd_bus *bus, const char *p, const char *i,
                         const char *pr, sd_bus_message *reply, void *u,
                         sd_bus_error *e)
{
    (void)bus; (void)p; (void)i; (void)pr; (void)u; (void)e;
    int rc = sd_bus_message_open_container(reply, 'a', "s");
    if (rc < 0) return rc;
    if (g_svc_uuid[0]) sd_bus_message_append(reply, "s", g_svc_uuid);
    return sd_bus_message_close_container(reply);
}

static int adv_get_local_name(sd_bus *bus, const char *p, const char *i,
                              const char *pr, sd_bus_message *reply, void *u,
                              sd_bus_error *e)
{
    (void)bus; (void)p; (void)i; (void)pr; (void)u; (void)e;
    return sd_bus_message_append(reply, "s", g_name);
}

/* Advertising interval.  Without these BlueZ leaves the kernel default of
 * 0x0800 = 1.28 s between advertisements.  That is what made hub-side
 * provisioning a coin flip: the hub's USB dongle hears this board at
 * about -80 dBm and decodes only ~25% of packets, so at 1.28 s it saw
 * gaps of 6-13 s (measured 2026-09-20, 7 adverts per minute at the hub
 * versus 33 at a second receiver in the same room), and Bleak's 15 s
 * scan-to-connect window then timed out about half the time.  Setup mode
 * lasts minutes and power is irrelevant, so advertise fast: 100-200 ms,
 * the same profile the gateway backend and the Zephyr peripheral use. */
static int adv_get_min_interval(sd_bus *bus, const char *p, const char *i,
                                const char *pr, sd_bus_message *r, void *u,
                                sd_bus_error *e)
{
    (void)bus; (void)p; (void)i; (void)pr; (void)u; (void)e;
    return sd_bus_message_append(r, "u", (uint32_t)100);
}

static int adv_get_max_interval(sd_bus *bus, const char *p, const char *i,
                                const char *pr, sd_bus_message *r, void *u,
                                sd_bus_error *e)
{
    (void)bus; (void)p; (void)i; (void)pr; (void)u; (void)e;
    return sd_bus_message_append(r, "u", (uint32_t)200);
}

static int adv_get_discoverable(sd_bus *bus, const char *p, const char *i,
                                const char *pr, sd_bus_message *reply, void *u,
                                sd_bus_error *e)
{
    (void)bus; (void)p; (void)i; (void)pr; (void)u; (void)e;
    return sd_bus_message_append(reply, "b", 1);
}

static int adv_release(sd_bus_message *m, void *u, sd_bus_error *e)
{
    (void)u; (void)e;
    g_adv_active = false;
    NN_LOG_WRN("BLE advertisement released by bluez");
    return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable adv_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Type",         "s",  adv_get_type,         0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ServiceUUIDs", "as", adv_get_uuids,        0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("LocalName",    "s",  adv_get_local_name,   0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MinInterval",  "u",  adv_get_min_interval, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MaxInterval",  "u",  adv_get_max_interval, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Discoverable", "b",  adv_get_discoverable, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Release", NULL, NULL, adv_release, 0),
    SD_BUS_VTABLE_END
};

/* Drop a connection slot and tell the app.
 *
 * BlueZ keeps a registered advertisement across a connection, and the
 * kernel resumes advertising once the central leaves, so after a
 * disconnect there is normally nothing to do.  Re-registering anyway
 * (seen in E2E run 2, 2026-09-14): this runs on the bus thread, so
 * advertise_start waited for a reply only this same thread could pump --
 * the whole bus stalled for reg_wait's 10 s, deaf to any central and to
 * BlueZ reading our advertisement -- and BlueZ finally answered
 * AlreadyExists.  So register again only if BlueZ actually Released the
 * advertisement, and never wait for it here. */
static void conn_dropped(const char *device)
{
    nn_pal_ble_conn_t id = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_CONNS; i++)
        if (g_conns[i].id && !strcmp(g_conns[i].device, device)) {
            id = g_conns[i].id;
            g_conns[i].id = 0;
            g_conns[i].device[0] = '\0';
            break;
        }
    nn_pal_ble_conn_cb_t cb = g_conn_cb;
    void *user = g_conn_user;
    pthread_mutex_unlock(&g_lock);
    if (!id) return;

    NN_LOG_INF("central disconnected (handle %u)", (unsigned)id);
    if (cb) {
        nn_pal_ble_conn_info_t info = {0};
        cb(id, NN_PAL_BLE_CONN_EV_DISCONNECTED, &info, user);
    }
    if (g_adv_wanted && !g_adv_active) {
        if (nn_pal_ble_advertise_start(NULL) == 0)
            NN_LOG_INF("re-registering the advertisement BlueZ released");
        else
            NN_LOG_WRN("could not re-register advertising after disconnect");
    }
}

/* org.bluez.Device1 Connected -> false.  Without this the app never
 * learns a central went away. */
static int device_props_changed(sd_bus_message *m, void *u, sd_bus_error *e)
{
    (void)u; (void)e;
    const char *iface = NULL;
    if (sd_bus_message_read_basic(m, 's', &iface) < 0) return 0;
    if (!iface || strcmp(iface, "org.bluez.Device1")) return 0;

    const char *path = sd_bus_message_get_path(m);
    if (!path) return 0;

    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char *key = NULL;
        if (sd_bus_message_read_basic(m, 's', &key) < 0) break;
        if (key && !strcmp(key, "Connected")) {
            int connected = 0;
            if (sd_bus_message_enter_container(m, 'v', "b") > 0) {
                sd_bus_message_read_basic(m, 'b', &connected);
                sd_bus_message_exit_container(m);
            }
            if (!connected) conn_dropped(path);
        } else {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    return 0;
}

/* ── bus loop ─────────────────────────────────────────────────────── */

/*
 * ONE mutex around every sd-bus touch.  An sd_bus is not thread-safe, and
 * this backend is used from two threads: the loop below, and whichever
 * thread calls the PAL (register/notify/advertise).  Without this the two
 * corrupt each other's message state and the symptom is a baffling
 * -EBADMSG out of sd_bus_process rather than anything that names a race.
 *
 * The loop must therefore NOT block inside sd-bus while holding the lock,
 * so it polls the bus fd itself instead of calling sd_bus_wait().
 */
/*
 * RECURSIVE, and that is load-bearing.  The bus thread holds this while
 * dispatching sd_bus_process, and a dispatched WriteValue runs the app's
 * write handler, which for nn_prov calls set_status() -> notify() ->
 * back in here to emit PropertiesChanged.  With a plain mutex that is a
 * self-deadlock: the handler never returns, so no D-Bus reply is ever
 * sent, bluetoothd times out after exactly 5 s and synthesises ATT 0x0e
 * to the central -- after the write has already been applied.  The bus
 * thread is then wedged, so nothing else is ever dispatched either (no
 * subsequent writes, no disconnect).  Re-entering from the same thread
 * is safe: sd-bus is fine with emitting a signal from inside a callback.
 */
static pthread_mutex_t g_bus_lock;      /* initialised recursive in _init */

/* Registration is async (see reg_done_cb): 0 pending, 1 ok, negative errno. */
static atomic_int g_reg_app_rc  = ATOMIC_VAR_INIT(0);
static atomic_int g_reg_adv_rc  = ATOMIC_VAR_INIT(0);

static int reg_done(sd_bus_message *m, atomic_int *slot, const char *what)
{
    const sd_bus_error *e = sd_bus_message_get_error(m);
    if (e) {
        NN_LOG_ERR("%s: %s: %s", what, e->name ? e->name : "?",
                   e->message ? e->message : "?");
        atomic_store(slot, -EIO);
    } else {
        atomic_store(slot, 1);
    }
    return 0;
}

static int reg_app_cb(sd_bus_message *m, void *u, sd_bus_error *e)
{
    (void)u; (void)e;
    return reg_done(m, &g_reg_app_rc, "RegisterApplication");
}

/* Completes every RegisterAdvertisement, including the ones nobody waits
 * for (sent from the bus thread).  AlreadyExists means BlueZ still holds
 * our advertisement -- exactly the state asked for, not a failure. */
static int reg_adv_cb(sd_bus_message *m, void *u, sd_bus_error *e)
{
    (void)u; (void)e;
    const sd_bus_error *err = sd_bus_message_get_error(m);
    if (err && err->name &&
        !strcmp(err->name, "org.bluez.Error.AlreadyExists"))
        atomic_store(&g_reg_adv_rc, 1);
    else
        reg_done(m, &g_reg_adv_rc, "RegisterAdvertisement");
    if (atomic_load(&g_reg_adv_rc) > 0 && !g_adv_active) {
        g_adv_active = true;
        NN_LOG_INF("advertising as '%s'", g_name);
    }
    return 0;
}

/* Wait for an async registration to land, pumped by the bus thread. */
static int reg_wait(atomic_int *slot, const char *what)
{
    for (int i = 0; i < 100; i++) {           /* up to ~10 s */
        int v = atomic_load(slot);
        if (v > 0) return 0;
        if (v < 0) return v;
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    NN_LOG_ERR("%s: timed out waiting for BlueZ", what);
    return -ETIMEDOUT;
}

static void *bus_thread(void *arg)
{
    (void)arg;
    while (atomic_load(&g_run)) {
        pthread_mutex_lock(&g_bus_lock);
        int rc = sd_bus_process(g_bus, NULL);
        int fd = rc <= 0 ? sd_bus_get_fd(g_bus) : -1;
        int ev = rc <= 0 ? sd_bus_get_events(g_bus) : 0;
        pthread_mutex_unlock(&g_bus_lock);

        if (rc < 0) { NN_LOG_ERR("sd_bus_process: %d", rc); break; }
        if (rc > 0) continue;                 /* more queued work */

        /* Block OUTSIDE the lock, on our own poll rather than
         * sd_bus_wait(), so PAL calls from other threads are not starved
         * for the length of the wait. */
        if (fd >= 0 && ev >= 0) {
            struct pollfd pfd = { .fd = fd, .events = (short)ev };
            int pr = poll(&pfd, 1, 200);      /* 200 ms, so shutdown is prompt */
            if (pr < 0 && errno != EINTR) {
                NN_LOG_ERR("poll: %d", errno);
                break;
            }
        } else {
            struct timespec ts = { 0, 50 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

/* ── lifecycle ────────────────────────────────────────────────────── */

int nn_pal_ble_init(void)
{
    if (g_bus) return 0;
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_bus_lock, &ma);
    pthread_mutexattr_destroy(&ma);

    int rc = sd_bus_open_system(&g_bus);
    if (rc < 0) { NN_LOG_ERR("sd_bus_open_system: %d", rc); return rc; }
    rc = sd_bus_add_object_manager(g_bus, NULL, APP_PATH);
    if (rc < 0) { NN_LOG_ERR("add_object_manager: %d", rc); return rc; }

    /* Power the adapter on; a cold boot often leaves it down and the
     * failure mode is a silent absence of advertising. */
    sd_bus_error err = SD_BUS_ERROR_NULL;
    rc = sd_bus_set_property(g_bus, "org.bluez", g_adapter,
                             "org.bluez.Adapter1", "Powered", &err, "b", 1);
    if (rc < 0) {
        NN_LOG_WRN("adapter Powered=1 failed (%s) — is %s present?",
                   err.message ? err.message : "?", g_adapter);
        sd_bus_error_free(&err);
    }

    rc = sd_bus_match_signal(g_bus, NULL, "org.bluez", NULL,
                             "org.freedesktop.DBus.Properties",
                             "PropertiesChanged", device_props_changed, NULL);
    if (rc < 0) NN_LOG_WRN("device match: %d (no disconnect events)", rc);

    atomic_store(&g_run, true);
    if (pthread_create(&g_thread, NULL, bus_thread, NULL) != 0) {
        atomic_store(&g_run, false);
        return -errno;
    }
    atomic_store(&g_ready, true);
    NN_LOG_INF("nn_pal BLE (BlueZ) up on %s", g_adapter);
    return 0;
}

int nn_pal_ble_shutdown(void)
{
    if (!g_bus) return 0;
    atomic_store(&g_run, false);
    pthread_join(g_thread, NULL);
    sd_bus_flush_close_unref(g_bus);
    g_bus = NULL;
    atomic_store(&g_ready, false);
    return 0;
}

bool nn_pal_ble_is_ready(void) { return atomic_load(&g_ready); }

int nn_pal_ble_set_device_name(const char *name)
{
    if (!name) return -EINVAL;
    snprintf(g_name, sizeof g_name, "%s", name);
    return 0;
}

int nn_pal_ble_gatt_register_service(nn_pal_ble_service_t *svc)
{
    if (!svc || !g_bus) return -EINVAL;
    if (svc->chrc_count > MAX_CHRCS) return -E2BIG;

    uuid_to_str(&svc->uuid, g_svc_uuid, sizeof g_svc_uuid);
    pthread_mutex_lock(&g_bus_lock);
    int rc = sd_bus_add_object_vtable(g_bus, NULL, SVC_PATH,
                                      "org.bluez.GattService1", svc_vtable, NULL);
    if (rc < 0) { pthread_mutex_unlock(&g_bus_lock);
                  NN_LOG_ERR("export service: %d", rc); return rc; }

    g_chrc_count = svc->chrc_count;
    for (size_t i = 0; i < svc->chrc_count; i++) {
        struct chrc_slot *s = &g_chrcs[i];
        s->def = &svc->chrcs[i];
        snprintf(s->path, sizeof s->path, SVC_PATH "/chrc%zu", i);
        uuid_to_str(&s->def->uuid, s->uuid_str, sizeof s->uuid_str);
        rc = sd_bus_add_object_vtable(g_bus, NULL, s->path,
                                      "org.bluez.GattCharacteristic1",
                                      chrc_vtable, NULL);
        if (rc < 0) { pthread_mutex_unlock(&g_bus_lock);
                      NN_LOG_ERR("export chrc %zu: %d", i, rc); return rc; }
        /* The PAL contract says the handle identifies the chrc for
         * notify(); index+1 keeps 0 reserved for "invalid". */
        s->def->handle = (uint16_t)(i + 1);
    }
    pthread_mutex_unlock(&g_bus_lock);

    /* MUST be async.  BlueZ calls ObjectManager.GetManagedObjects back
     * into us BEFORE it replies to RegisterApplication; sd_bus_call()
     * dispatches only replies and signals while it waits, never incoming
     * METHOD CALLS, so a synchronous call deadlocks -- or, with a bus
     * thread also running, corrupts message state and returns -EBADMSG.
     * Fire it off and let the bus loop serve GetManagedObjects. */
    atomic_store(&g_reg_app_rc, 0);
    pthread_mutex_lock(&g_bus_lock);
    rc = sd_bus_call_method_async(g_bus, NULL, "org.bluez", g_adapter,
                                  "org.bluez.GattManager1",
                                  "RegisterApplication", reg_app_cb, NULL,
                                  "oa{sv}", APP_PATH, 0);
    pthread_mutex_unlock(&g_bus_lock);
    if (rc < 0) { NN_LOG_ERR("RegisterApplication send: %d", rc); return rc; }

    rc = reg_wait(&g_reg_app_rc, "RegisterApplication");
    if (rc < 0) return rc;
    NN_LOG_INF("GATT service %s registered (%zu chrcs)", g_svc_uuid,
               svc->chrc_count);
    return 0;
}

int nn_pal_ble_gatt_notify(nn_pal_ble_conn_t conn, uint16_t chrc_handle,
                           const void *data, size_t len)
{
    (void)conn;
    if (!g_bus || !data) return -EINVAL;
    if (chrc_handle == 0 || chrc_handle > g_chrc_count) return -EINVAL;
    struct chrc_slot *s = &g_chrcs[chrc_handle - 1];
    if (!s->notifying) return -EINVAL;          /* nobody subscribed */
    if (len > sizeof s->last_value) return -E2BIG;

    memcpy(s->last_value, data, len);
    s->last_len = len;
    /* BlueZ delivers a notification by emitting PropertiesChanged on the
     * characteristic's Value. */
    pthread_mutex_lock(&g_bus_lock);
    int nrc = sd_bus_emit_properties_changed(g_bus, s->path,
                                             "org.bluez.GattCharacteristic1",
                                             "Value", NULL);
    pthread_mutex_unlock(&g_bus_lock);
    return nrc;
}

int nn_pal_ble_advertise_start(const nn_pal_ble_adv_params_t *p)
{
    if (!g_bus) return -EINVAL;
    if (p) g_adv = *p;
    g_adv_wanted = true;
    if (g_adv_active) return 0;

    pthread_mutex_lock(&g_bus_lock);
    int rc = sd_bus_add_object_vtable(g_bus, NULL, ADV_PATH,
                                      "org.bluez.LEAdvertisement1",
                                      adv_vtable, NULL);
    pthread_mutex_unlock(&g_bus_lock);
    if (rc < 0 && rc != -EEXIST) { NN_LOG_ERR("export adv: %d", rc); return rc; }

    /* Async for the same reason as RegisterApplication: BlueZ reads our
     * LEAdvertisement1 properties before replying. */
    atomic_store(&g_reg_adv_rc, 0);
    pthread_mutex_lock(&g_bus_lock);
    rc = sd_bus_call_method_async(g_bus, NULL, "org.bluez", g_adapter,
                                  "org.bluez.LEAdvertisingManager1",
                                  "RegisterAdvertisement", reg_adv_cb, NULL,
                                  "oa{sv}", ADV_PATH, 0);
    pthread_mutex_unlock(&g_bus_lock);
    if (rc < 0) { NN_LOG_ERR("RegisterAdvertisement send: %d", rc); return rc; }

    /* On the bus thread (a disconnect or write handler) nothing else can
     * pump the reply: waiting would stall the whole bus, BlueZ's read of
     * our advertisement properties included, until reg_wait gives up.
     * reg_adv_cb finishes the job when the loop gets back to it. */
    if (pthread_equal(pthread_self(), g_thread)) return 0;
    return reg_wait(&g_reg_adv_rc, "RegisterAdvertisement");
}

int nn_pal_ble_advertise_stop(void)
{
    g_adv_wanted = false;
    if (!g_bus || !g_adv_active) return 0;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    pthread_mutex_lock(&g_bus_lock);        /* sd-bus is not thread-safe */
    int rc = sd_bus_call_method(g_bus, "org.bluez", g_adapter,
                               "org.bluez.LEAdvertisingManager1",
                               "UnregisterAdvertisement", &err, NULL,
                               "o", ADV_PATH);
    pthread_mutex_unlock(&g_bus_lock);
    if (rc < 0) NN_LOG_WRN("UnregisterAdvertisement: %s",
                           err.message ? err.message : strerror(-rc));
    sd_bus_error_free(&err);
    g_adv_active = false;
    return 0;
}

int nn_pal_ble_conn_cb_register(nn_pal_ble_conn_cb_t cb, void *user)
{
    g_conn_cb = cb;
    g_conn_user = user;
    return 0;
}

int nn_pal_ble_conn_disconnect(nn_pal_ble_conn_t conn)
{
    if (!g_bus || conn == 0 || conn > MAX_CONNS) return -EINVAL;
    struct conn_slot *c = &g_conns[conn - 1];
    if (!c->id) return -ENOTCONN;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int rc = sd_bus_call_method(g_bus, "org.bluez", c->device,
                                "org.bluez.Device1", "Disconnect",
                                &err, NULL, "");
    if (rc < 0) NN_LOG_WRN("Disconnect: %s",
                           err.message ? err.message : strerror(-rc));
    sd_bus_error_free(&err);
    pthread_mutex_lock(&g_lock);
    c->id = 0; c->device[0] = '\0';
    pthread_mutex_unlock(&g_lock);
    return rc < 0 ? rc : 0;
}

/* ── central role: not supported here ─────────────────────────────
 * Deliberately stubbed rather than half-implemented.  Setup mode is a
 * peripheral-only job, and the hub does the central side.
 */
int nn_pal_ble_scan_start(const nn_pal_ble_scan_params_t *params,
                          nn_pal_ble_scan_cb_t cb, void *user)
{ (void)params; (void)cb; (void)user; return -ENOTSUP; }
int nn_pal_ble_scan_stop(void) { return -ENOTSUP; }
int nn_pal_ble_connect(const uint8_t addr[6], uint8_t addr_type)
{ (void)addr; (void)addr_type; return -ENOTSUP; }
int nn_pal_ble_gatt_mtu_exchange(nn_pal_ble_conn_t conn)
{ (void)conn; return -ENOTSUP; }
int nn_pal_ble_set_pairing_mode(nn_pal_ble_pairing_mode_t m)
{ (void)m; return -ENOTSUP; }
