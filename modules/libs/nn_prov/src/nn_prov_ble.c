/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_prov_ble — provisioning GATT peripheral, built on nn_pal/ble (NimBLE).
 *
 * Service e7f00001-6b3e-4f6b-9232-3e26d0d5a2f0:
 *   DEVICE_PUBKEY e7f00005  READ          → 32B device X25519 public key
 *   CONFIG        e7f00004  WRITE         → plaintext config blob
 *   WIFI          e7f00006  WRITE         → ECIES v2 envelope {ssid,pass}
 *   STATUS        e7f00003  READ + NOTIFY → 1-byte nn_prov_status_t
 *   FW_NAME       e7f00007  READ          → "<image> <version>" utf-8
 *
 * Just-Works (no bonding); the hub is the BLE central.  The GATT table and
 * advertising now go through nn_pal/ble — no direct NimBLE calls here.
 */
#include "nn_prov/nn_prov.h"
#include "nn_prov_internal.h"
#include "nn_prov_crypto.h"

#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_prov_ble);
#include <nn_pal/ble.h>
#include <string.h>
#include <stdio.h>
#include "esp_app_desc.h"

#define DEVICE_NAME "nn-media-net"

/* Base ..-6b3e-4f6b-9232-3e26d0d5a2f0; 4th byte selects the characteristic. */
#define PROV_UUID_STR(b3) "e7f000" b3 "-6b3e-4f6b-9232-3e26d0d5a2f0"

static nn_pal_ble_conn_t s_conn;                 /* 0 = no central */
static nn_pal_ble_service_t s_svc;
static nn_pal_ble_chrc_t    s_chrcs[5];

/* ── characteristic callbacks ───────────────────────────────────────────── */
static int devpub_read(const nn_pal_ble_read_ctx_t *c, void *u,
                       uint8_t *out, size_t cap, size_t *n)
{
    (void)c; (void)u;
    if (cap < 32) return -1;
    nn_prov_crypto_device_pub(out);
    *n = 32;
    return 0;
}
/* FW_NAME (e7f00007): "<image> <version>", straight from the app
 * descriptor — e.g. "nn-app-camera-esp32p4module-sdio-ov5647 b42f83e".
 * Read by the hub during a provisioning scan so the operator sees what
 * they are adopting, and the OTA catalog key is known up front. */
static int fwname_read(const nn_pal_ble_read_ctx_t *c, void *u,
                       uint8_t *out, size_t cap, size_t *n)
{
    (void)c; (void)u;
    const esp_app_desc_t *d = esp_app_get_description();
    int len = snprintf((char *)out, cap, "%s %s", d->project_name, d->version);
    if (len < 0 || (size_t)len >= cap) return -1;
    *n = (size_t)len;
    return 0;
}
static int status_read(const nn_pal_ble_read_ctx_t *c, void *u,
                       uint8_t *out, size_t cap, size_t *n)
{
    (void)c; (void)u;
    if (cap < 1) return -1;
    out[0] = (uint8_t)nn_prov_current_status();
    *n = 1;
    return 0;
}
static int config_write(const nn_pal_ble_write_ctx_t *c, void *u,
                        const uint8_t *buf, size_t len)
{
    (void)c; (void)u;
    nn_prov_handle_config(buf, len);
    return 0;
}
static int wifi_write(const nn_pal_ble_write_ctx_t *c, void *u,
                      const uint8_t *buf, size_t len)
{
    (void)c; (void)u;
    nn_prov_handle_wifi(buf, len);
    return 0;
}

/* ── status notify (called by the core on state change) ─────────────────── */
void nn_prov_ble_notify_status(nn_prov_status_t st)
{
    if (!s_conn) return;
    uint8_t v = (uint8_t)st;
    nn_pal_ble_gatt_notify(s_conn, s_chrcs[3].handle, &v, 1);   /* [3] = STATUS */
}

/* ── connection events ──────────────────────────────────────────────────── */
static void conn_cb(nn_pal_ble_conn_t conn, nn_pal_ble_conn_event_t ev,
                    const nn_pal_ble_conn_info_t *info, void *user)
{
    (void)user;
    switch (ev) {
    case NN_PAL_BLE_CONN_EV_CONNECTED:
        s_conn = conn;
        NN_LOG_INF("central connected (handle %u)", conn);
        break;
    case NN_PAL_BLE_CONN_EV_DISCONNECTED:
        NN_LOG_INF("central disconnected (reason 0x%x); re-advertising",
                   info ? info->disconnect_reason : 0);
        s_conn = 0;     /* nn_pal re-arms advertising internally */
        break;
    case NN_PAL_BLE_CONN_EV_MTU_UPDATED:
        NN_LOG_INF("MTU update: %u", info ? info->mtu : 0);
        break;
    default:
        break;
    }
}

esp_err_t nn_prov_ble_start(void)
{
    if (nn_pal_ble_init() != 0) { NN_LOG_ERR("ble_init failed"); return ESP_FAIL; }
    nn_pal_ble_set_device_name(DEVICE_NAME);

    nn_pal_ble_uuid_t svc_uuid;
    nn_pal_ble_uuid_from_str(&svc_uuid, PROV_UUID_STR("01"));
    nn_pal_ble_uuid_from_str(&s_chrcs[0].uuid, PROV_UUID_STR("05"));   /* DEVPUB */
    s_chrcs[0].flags = NN_PAL_BLE_CHRC_READ;  s_chrcs[0].on_read = devpub_read;
    nn_pal_ble_uuid_from_str(&s_chrcs[1].uuid, PROV_UUID_STR("04"));   /* CONFIG */
    s_chrcs[1].flags = NN_PAL_BLE_CHRC_WRITE; s_chrcs[1].on_write = config_write;
    nn_pal_ble_uuid_from_str(&s_chrcs[2].uuid, PROV_UUID_STR("06"));   /* WIFI   */
    s_chrcs[2].flags = NN_PAL_BLE_CHRC_WRITE; s_chrcs[2].on_write = wifi_write;
    nn_pal_ble_uuid_from_str(&s_chrcs[3].uuid, PROV_UUID_STR("03"));   /* STATUS */
    s_chrcs[3].flags = NN_PAL_BLE_CHRC_READ | NN_PAL_BLE_CHRC_NOTIFY;
    s_chrcs[3].on_read = status_read;
    nn_pal_ble_uuid_from_str(&s_chrcs[4].uuid, PROV_UUID_STR("07"));   /* FW_NAME */
    s_chrcs[4].flags = NN_PAL_BLE_CHRC_READ;
    s_chrcs[4].on_read = fwname_read;

    s_svc.uuid = svc_uuid;
    s_svc.chrcs = s_chrcs;
    s_svc.chrc_count = 5;
    if (nn_pal_ble_gatt_register_service(&s_svc) != 0) {
        NN_LOG_ERR("register_service failed");
        return ESP_FAIL;
    }
    nn_pal_ble_conn_cb_register(conn_cb, NULL);

    nn_pal_ble_adv_params_t adv = {
        .connectable = true,
        .service_uuid = &svc_uuid,   /* copied into the backend */
    };
    nn_pal_ble_advertise_start(&adv);   /* starts the NimBLE host task */
    NN_LOG_INF("provisioning service started (via nn_pal/ble)");
    return ESP_OK;
}
