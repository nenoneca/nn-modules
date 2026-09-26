/* SPDX-License-Identifier: Apache-2.0 */
#include "nn_ota/nn_ota_c6.h"
#include "nn_ota/nn_ota_dfu.h"
#include "nn_link/nn_link.h"
#include "nn_link/nn_ota_link.h"

#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_ota_c6);
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

#define NVS_NS  "nn_ota"

/* ── P4 round-trip state (the control channel serialises OTA ops, so a single
 *    outstanding P4 request at a time is sufficient) ──────────────────────── */
static SemaphoreHandle_t s_ack_sem, s_cksum_sem, s_ver_sem;
static volatile int      s_ack_status;
static uint8_t           s_cksum[NN_OTA_SHA_LEN];
static char              s_p4_version[32];

/* staging session state */
static char    s_p4_target[32], s_c6_target[32];
static bool    s_p4_staged, s_c6_staged;
static enum { ST_IDLE, ST_STAGING, ST_ARMED, ST_APPLYING } s_state;

/* ── inbound P4 -> C6 ────────────────────────────────────────────────────── */
void nn_ota_c6_on_p4_msg(const uint8_t *data, size_t len)
{
    if (len < (size_t)NN_OTA_HDR_LEN) return;
    const nn_ota_hdr_t *h = (const nn_ota_hdr_t *)data;
    const uint8_t *pay = data + NN_OTA_HDR_LEN;
    size_t plen = len - NN_OTA_HDR_LEN;

    switch (h->type) {
    case NN_OTA_MSG_VERSION: {
        size_t n = plen < sizeof s_p4_version - 1 ? plen : sizeof s_p4_version - 1;
        memcpy(s_p4_version, pay, n); s_p4_version[n] = '\0';
        NN_LOG_INF("P4 version = '%s'", s_p4_version);
        if (s_ver_sem) xSemaphoreGive(s_ver_sem);
        break;
    }
    case NN_OTA_MSG_CKSUM:
        if (plen >= NN_OTA_SHA_LEN) memcpy(s_cksum, pay, NN_OTA_SHA_LEN);
        if (s_cksum_sem) xSemaphoreGive(s_cksum_sem);
        break;
    case NN_OTA_MSG_ACK:
        s_ack_status = (int)h->arg;
        if (s_ack_sem) xSemaphoreGive(s_ack_sem);
        break;
    default:
        break;
    }
}

/* ── outbound to P4 + wait ───────────────────────────────────────────────── */
static esp_err_t p4_send(uint8_t type, uint32_t arg, const void *pl, size_t pn)
{
    uint8_t pkt[NN_LINK_MAX_PACKET];
    if (pn > NN_OTA_MAX_PAYLOAD) pn = NN_OTA_MAX_PAYLOAD;
    nn_ota_hdr_t *h = (nn_ota_hdr_t *)pkt;
    h->magic = NN_OTA_MAGIC; h->type = type; h->rsv = 0; h->arg = arg;
    if (pl && pn) memcpy(pkt + NN_OTA_HDR_LEN, pl, pn);
    return nn_link_send(pkt, NN_OTA_HDR_LEN + pn);
}

/* Send a request and wait for the P4's ACK; retry on loss.  The C6->P4
 * (slave->master) link drops the odd packet under tight request/reply, so we
 * resend.  Resending the SAME offset is safe: the P4 buffers the target sector
 * in RAM and just re-fills that position before it flushes (we never advance to
 * the next chunk until this one is acked, so a retry never goes backward). */
static int p4_req_ack_n(uint8_t type, uint32_t arg, const void *pl, size_t pn, int to_ms, int tries)
{
    for (int i = 0; i < tries; i++) {
        xSemaphoreTake(s_ack_sem, 0);         /* drain any stale ACK */
        if (p4_send(type, arg, pl, pn) != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(to_ms)) == pdTRUE) return s_ack_status;
        if (i + 1 < tries) NN_LOG_WRN("P4 msg 0x%02x arg=%u: no ACK, retry %d/%d",
                                    type, (unsigned)arg, i + 1, tries);
    }
    return -1001;
}
static int p4_req_ack(uint8_t type, uint32_t arg, const void *pl, size_t pn, int to_ms)
{
    return p4_req_ack_n(type, arg, pl, pn, to_ms, 1);
}

/* ── control ops (from nn_ctrl dispatch) ─────────────────────────────────── */
int nn_ota_c6_offer(const uint8_t *body, size_t n)
{
    /* body = "<p4_ver>\n<c6_ver>" */
    char tmp[80]; size_t k = n < sizeof tmp - 1 ? n : sizeof tmp - 1;
    memcpy(tmp, body, k); tmp[k] = '\0';
    char *nl = strchr(tmp, '\n');
    if (!nl) return 1;
    *nl = '\0';
    strlcpy(s_p4_target, tmp, sizeof s_p4_target);
    strlcpy(s_c6_target, nl + 1, sizeof s_c6_target);
    s_p4_staged = s_c6_staged = false;
    s_state = ST_STAGING;
    NN_LOG_INF("OFFER p4='%s' c6='%s'", s_p4_target, s_c6_target);
    return 0;
}

int nn_ota_c6_begin(uint8_t target, uint32_t size)
{
    if (target == NN_OTA_TARGET_C6) {
        return nn_ota_dfu_begin(size) == ESP_OK ? 0 : 2;
    }
    int st = p4_req_ack_n(NN_OTA_MSG_BEGIN, size, NULL, 0, 4000, 4);
    NN_LOG_INF("P4 BEGIN size=%u -> %d", (unsigned)size, st);
    return st == 0 ? 0 : 2;
}

int nn_ota_c6_data(uint8_t target, uint32_t off, const uint8_t *b, size_t n)
{
    if (target == NN_OTA_TARGET_C6) {
        return nn_ota_dfu_write(off, b, n) == ESP_OK ? 0 : 3;
    }
    int st = p4_req_ack_n(NN_OTA_MSG_CHUNK, off, b, n, 1500, 6);
    return st == 0 ? 0 : 3;
}

int nn_ota_c6_verify(uint8_t target, uint32_t size, const uint8_t sha[32])
{
    uint8_t got[NN_OTA_SHA_LEN];
    if (target == NN_OTA_TARGET_C6) {
        if (nn_ota_dfu_sha256(size, got) != ESP_OK) return -1;
    } else {
        bool ok = false;
        for (int i = 0; i < 4 && !ok; i++) {
            xSemaphoreTake(s_cksum_sem, 0);
            if (p4_send(NN_OTA_MSG_CKSUM_REQ, size, NULL, 0) != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            if (xSemaphoreTake(s_cksum_sem, pdMS_TO_TICKS(8000)) == pdTRUE) ok = true;
            else NN_LOG_WRN("P4 CKSUM: no reply, retry %d", i + 1);
        }
        if (!ok) return -1;
        memcpy(got, s_cksum, NN_OTA_SHA_LEN);
    }
    bool match = memcmp(got, sha, NN_OTA_SHA_LEN) == 0;
    if (match) { if (target == NN_OTA_TARGET_C6) s_c6_staged = true; else s_p4_staged = true; }
    NN_LOG_INF("VERIFY %s -> %s", target == NN_OTA_TARGET_C6 ? "C6" : "P4",
             match ? "MATCH" : "MISMATCH");
    return match ? 0 : 1;
}

int nn_ota_c6_arm(void)
{
    if (!s_p4_staged || !s_c6_staged) {
        NN_LOG_WRN("ARM rejected: p4_staged=%d c6_staged=%d", s_p4_staged, s_c6_staged);
        return 1;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "p4_armed", s_p4_target);
        nvs_set_str(h, "c6_armed", s_c6_target);
        nvs_commit(h); nvs_close(h);
    }
    s_state = ST_ARMED;
    NN_LOG_WRN("ARMED p4='%s' c6='%s' (await APPLY)", s_p4_target, s_c6_target);
    return 0;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));      /* let the APPLY reply leave the wire */
    NN_LOG_WRN("self-applying C6 swap — rebooting");
    esp_restart();
}

int nn_ota_c6_apply(void)
{
    if (s_state != ST_ARMED) { NN_LOG_WRN("APPLY but not armed"); return 1; }
    s_state = ST_APPLYING;

    /* 1) P4 first: command swap, wait for it to come back on target, confirm. */
    if (s_p4_staged) {
        int st = p4_req_ack_n(NN_OTA_MSG_APPLY, 0, NULL, 0, 4000, 4);
        if (st != 0) { NN_LOG_ERR("P4 APPLY ack=%d", st); return 2; }
        /* Clear the cached pre-swap version: only a FRESH report from the
         * rebooted P4 counts as reaching target. */
        strlcpy(s_p4_version, "?", sizeof s_p4_version);
        NN_LOG_WRN("P4 rebooting; passively listening for target '%s'", s_p4_target);
        /* Just LISTEN for the P4's unsolicited announce (it re-announces every
         * ~1.8 s after its swap-reboot).  Do NOT send VERSION_REQ here: a
         * slave->master send while the master is mid-reboot wedges the link and
         * blocks the very RX path we need to hear the announce on. */
        bool ok = false;
        for (int i = 0; i < 60 && !ok; i++) {        /* ~60 s budget */
            if (xSemaphoreTake(s_ver_sem, pdMS_TO_TICKS(1000)) == pdTRUE) {
                ok = (strcmp(s_p4_version, s_p4_target) == 0);
                if (!ok) NN_LOG_WRN("P4 reports '%s' (want '%s')", s_p4_version, s_p4_target);
            }
        }
        if (!ok) { NN_LOG_ERR("P4 did not reach '%s' (now '%s') — aborting, C6 stays good",
                            s_p4_target, s_p4_version); return 3; }
        int cst = p4_req_ack_n(NN_OTA_MSG_CONFIRM, 0, NULL, 0, 4000, 4);
        NN_LOG_WRN("P4 on target; CONFIRM ack=%d", cst);
    }

    /* 2) C6 self last: set boot + schedule reboot (so it can recover P4 first). */
    if (s_c6_staged) {
        if (nn_ota_dfu_set_boot() != ESP_OK) return 4;
        xTaskCreate(reboot_task, "nn_ota_reboot", 2048, NULL, 6, NULL);
    }
    return 0;
}

size_t nn_ota_c6_status(char *buf, size_t cap)
{
    /* If we don't yet know the P4's version (e.g. just rebooted), ask now so
     * the status reflects reality. Best-effort: skips if the link is down. */
    if (s_p4_version[0] == '\0' || strcmp(s_p4_version, "?") == 0) {
        for (int i = 0; i < 3 && (s_p4_version[0] == '\0' || s_p4_version[0] == '?'); i++) {
            xSemaphoreTake(s_ver_sem, 0);
            if (p4_send(NN_OTA_MSG_VERSION_REQ, 0, NULL, 0) == ESP_OK)
                xSemaphoreTake(s_ver_sem, pdMS_TO_TICKS(1000));
            else vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    char run[32]; nn_ota_dfu_running_version(run, sizeof run);
    const char *st = s_state == ST_IDLE ? "idle" : s_state == ST_STAGING ? "staging"
                   : s_state == ST_ARMED ? "armed" : "applying";
    return (size_t)snprintf(buf, cap,
        "{\"state\":\"%s\",\"c6\":\"%s\",\"p4\":\"%s\","
        "\"c6_target\":\"%s\",\"p4_target\":\"%s\","
        "\"p4_staged\":%d,\"c6_staged\":%d}",
        st, run, s_p4_version, s_c6_target, s_p4_target, s_p4_staged, s_c6_staged);
}

/* ── boot reconcile: self-confirm a successful C6 swap ───────────────────── */
static void reconcile(void)
{
    char run[32]; nn_ota_dfu_running_version(run, sizeof run);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char armed[32]; size_t n = sizeof armed;
    if (nvs_get_str(h, "c6_armed", armed, &n) == ESP_OK && armed[0]) {
        if (strcmp(run, armed) == 0) {
            NN_LOG_WRN("booted target '%s' — confirming (cancel rollback)", run);
            nn_ota_dfu_mark_valid();
            nvs_erase_key(h, "c6_armed");
            nvs_erase_key(h, "p4_armed");
            nvs_commit(h);
        } else {
            NN_LOG_WRN("armed '%s' but running '%s' (swap not applied yet)", armed, run);
        }
    }
    nvs_close(h);
}

void nn_ota_c6_init(void)
{
    s_ack_sem   = xSemaphoreCreateBinary();
    s_cksum_sem = xSemaphoreCreateBinary();
    s_ver_sem   = xSemaphoreCreateBinary();
    s_state = ST_IDLE;
    strlcpy(s_p4_version, "?", sizeof s_p4_version);
    reconcile();
}
