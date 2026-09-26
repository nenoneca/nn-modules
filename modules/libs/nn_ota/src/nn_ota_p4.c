/* SPDX-License-Identifier: Apache-2.0 */
#include "nn_ota/nn_ota_p4.h"
#include "nn_ota/nn_ota_dfu.h"
#include "nn_link/nn_link.h"
#include "nn_link/nn_ota_link.h"

#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_ota_p4);
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>


/* ── outbound helpers (P4 -> C6) ─────────────────────────────────────────── */
static void send_hdr_payload(uint8_t type, uint32_t arg,
                             const void *payload, size_t plen)
{
    uint8_t pkt[NN_LINK_MAX_PACKET];
    if (plen > NN_OTA_MAX_PAYLOAD) plen = NN_OTA_MAX_PAYLOAD;
    nn_ota_hdr_t *h = (nn_ota_hdr_t *)pkt;
    h->magic = NN_OTA_MAGIC;
    h->type  = type;
    h->rsv   = 0;
    h->arg   = arg;
    if (payload && plen) memcpy(pkt + NN_OTA_HDR_LEN, payload, plen);
    nn_link_send(pkt, NN_OTA_HDR_LEN + plen);
}

static void send_ack(int status)
{
    send_hdr_payload(NN_OTA_MSG_ACK, (uint32_t)status, NULL, 0);
}

static void send_version(void)
{
    char ver[32];
    nn_ota_dfu_running_version(ver, sizeof ver);
    send_hdr_payload(NN_OTA_MSG_VERSION, 0, ver, strlen(ver));
    NN_LOG_INF("announce version '%s'", ver);
}

/* ── inbound dispatch (C6 -> P4) ─────────────────────────────────────────── */
void nn_ota_p4_on_msg(const uint8_t *data, size_t len)
{
    if (len < (size_t)NN_OTA_HDR_LEN) return;
    const nn_ota_hdr_t *h = (const nn_ota_hdr_t *)data;
    const uint8_t *pay = data + NN_OTA_HDR_LEN;
    size_t plen = len - NN_OTA_HDR_LEN;

    switch (h->type) {
    case NN_OTA_MSG_VERSION_REQ:
        send_version();
        break;

    case NN_OTA_MSG_BEGIN: {
        esp_err_t err = nn_ota_dfu_begin(h->arg);
        NN_LOG_INF("BEGIN size=%u -> %s", (unsigned)h->arg, esp_err_to_name(err));
        send_ack(err);
        break;
    }
    case NN_OTA_MSG_CHUNK: {
        esp_err_t err = nn_ota_dfu_write(h->arg, pay, plen);
        if (err != ESP_OK) NN_LOG_ERR("CHUNK @%u (%u B): %s",
                                    (unsigned)h->arg, (unsigned)plen, esp_err_to_name(err));
        send_ack(err);
        break;
    }
    case NN_OTA_MSG_CKSUM_REQ: {
        uint8_t sha[NN_OTA_SHA_LEN];
        esp_err_t err = nn_ota_dfu_sha256(h->arg, sha);
        if (err != ESP_OK) { NN_LOG_ERR("CKSUM_REQ: %s", esp_err_to_name(err)); send_ack(err); break; }
        send_hdr_payload(NN_OTA_MSG_CKSUM, 0, sha, sizeof sha);
        NN_LOG_INF("CKSUM over %u B sent", (unsigned)h->arg);
        break;
    }
    case NN_OTA_MSG_APPLY: {
        esp_err_t err = nn_ota_dfu_set_boot();
        send_ack(err);
        if (err == ESP_OK) {
            NN_LOG_WRN("APPLY ok — rebooting into staged slot");
            vTaskDelay(pdMS_TO_TICKS(300));   /* let the ACK leave the wire */
            esp_restart();
        }
        break;
    }
    case NN_OTA_MSG_CONFIRM: {
        esp_err_t err = nn_ota_dfu_mark_valid();
        NN_LOG_WRN("CONFIRM -> mark_valid: %s", esp_err_to_name(err));
        send_ack(err);
        break;
    }
    default:
        NN_LOG_WRN("unknown OTA msg type 0x%02x", h->type);
        break;
    }
}

/* ── announce task: report version once the link is up ───────────────────── */
static void announce_task(void *arg)
{
    (void)arg;
    bool pending = nn_ota_dfu_pending_verify();
    if (pending) {
        NN_LOG_WRN("running PENDING_VERIFY image — announcing until C6 CONFIRM");
    }
    /* Announce repeatedly for ~40 s after boot: the C6 (which may have just
     * (re)established the link after our swap-reboot) needs a FRESH version to
     * confirm a swap — a single announce can be lost or beat the link coming up.
     * A pending-verify image keeps announcing longer (until confirmed) so the
     * C6 can always catch it within its apply window. */
    for (int i = 0; i < 24; i++) {
        nn_link_wait_connected(-1);
        send_version();
        vTaskDelay(pdMS_TO_TICKS(1800));
    }
    vTaskDelete(NULL);
}

void nn_ota_p4_init(void)
{
    xTaskCreate(announce_task, "nn_ota_ann", 3072, NULL, 5, NULL);
}
