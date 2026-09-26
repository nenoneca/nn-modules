/* SPDX-License-Identifier: Apache-2.0 */
#include "nn_ota/nn_ota_dfu.h"

#include <nn_pal/dfu.h>
#include "esp_app_desc.h"          /* running-image version string only */
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_ota_dfu);
#include "psa/crypto.h"
#include <string.h>


#define SECTOR  NN_PAL_DFU_SECTOR_SIZE

/* The staging target is the SECONDARY slot (nn_pal slot 1); its size bounds the
 * image.  We buffer a sector at a time, then erase+write it via nn_pal_dfu. */
static uint32_t s_slot_size;              /* secondary slot size (0=not begun) */
static uint8_t  s_sec[SECTOR];            /* current sector assembly buf       */
static int64_t  s_cur;                    /* current sector index (-1=none)    */
static bool     s_dirty;

esp_err_t nn_ota_dfu_begin(size_t total_size)
{
    s_slot_size = nn_pal_dfu_slot1_size();
    if (!s_slot_size) { NN_LOG_ERR("no secondary slot"); return ESP_ERR_NOT_FOUND; }
    if (total_size > s_slot_size) {
        NN_LOG_ERR("image %u > slot %u", (unsigned)total_size, (unsigned)s_slot_size);
        return ESP_ERR_INVALID_SIZE;
    }
    s_cur   = -1;
    s_dirty = false;
    memset(s_sec, 0xFF, SECTOR);
    NN_LOG_INF("begin: stage %u B into secondary slot (%u B)",
             (unsigned)total_size, (unsigned)s_slot_size);
    return ESP_OK;
}

/* Erase + write the currently buffered sector. */
esp_err_t nn_ota_dfu_flush(void)
{
    if (!s_slot_size || s_cur < 0 || !s_dirty) return ESP_OK;
    uint32_t off = (uint32_t)s_cur * SECTOR;
    if (nn_pal_dfu_slot1_erase(off, SECTOR)) { NN_LOG_ERR("erase @0x%x", (unsigned)off); return ESP_FAIL; }
    if (nn_pal_dfu_slot1_write(off, s_sec, SECTOR)) { NN_LOG_ERR("write @0x%x", (unsigned)off); return ESP_FAIL; }
    s_dirty = false;
    return ESP_OK;
}

esp_err_t nn_ota_dfu_write(size_t offset, const void *data, size_t len)
{
    if (!s_slot_size) return ESP_ERR_INVALID_STATE;
    if (offset + len > s_slot_size) return ESP_ERR_INVALID_SIZE;
    const uint8_t *p = data;
    while (len) {
        int64_t sec = offset / SECTOR;
        size_t  pos = offset % SECTOR;
        size_t  n   = SECTOR - pos;
        if (n > len) n = len;
        if (sec != s_cur) {
            esp_err_t err = nn_ota_dfu_flush();   /* commit the previous sector */
            if (err != ESP_OK) return err;
            s_cur = sec;
            memset(s_sec, 0xFF, SECTOR);          /* fresh erased-state buffer  */
        }
        memcpy(s_sec + pos, p, n);
        s_dirty = true;
        offset += n; p += n; len -= n;
    }
    return ESP_OK;
}

esp_err_t nn_ota_dfu_sha256(size_t size, uint8_t out[32])
{
    if (!s_slot_size) return ESP_ERR_INVALID_STATE;
    esp_err_t err = nn_ota_dfu_flush();
    if (err != ESP_OK) return err;

    psa_crypto_init();   /* idempotent */
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) return ESP_FAIL;

    static uint8_t buf[SECTOR];
    size_t off = 0;
    while (off < size) {
        size_t n = size - off;
        if (n > sizeof buf) n = sizeof buf;
        if (nn_pal_dfu_slot_read(NN_PAL_DFU_SLOT_SECONDARY, off, buf, n)) { psa_hash_abort(&op); return ESP_FAIL; }
        if (psa_hash_update(&op, buf, n) != PSA_SUCCESS) { psa_hash_abort(&op); return ESP_FAIL; }
        off += n;
    }
    size_t olen = 0;
    if (psa_hash_finish(&op, out, 32, &olen) != PSA_SUCCESS) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t nn_ota_dfu_set_boot(void)
{
    if (!s_slot_size) return ESP_ERR_INVALID_STATE;
    esp_err_t err = nn_ota_dfu_flush();
    if (err != ESP_OK) return err;
    if (nn_pal_dfu_request_upgrade(false)) {   /* TEST mode: rollback armed */
        NN_LOG_ERR("request_upgrade failed");
        return ESP_FAIL;
    }
    NN_LOG_WRN("next boot = secondary slot (TEST mode)");
    return ESP_OK;
}

esp_err_t nn_ota_dfu_mark_valid(void)
{
    int rc = nn_pal_dfu_confirm();
    NN_LOG_INF("mark_valid: %d", rc);
    return rc ? ESP_FAIL : ESP_OK;
}

void nn_ota_dfu_running_version(char *buf, size_t n)
{
    if (!buf || n == 0) return;
    const esp_app_desc_t *d = esp_app_get_description();
    strncpy(buf, d ? d->version : "?", n - 1);
    buf[n - 1] = '\0';
}

bool nn_ota_dfu_pending_verify(void)
{
    nn_pal_dfu_state_t st;
    return nn_pal_dfu_get_state(&st) == 0 && st == NN_PAL_DFU_STATE_RUNNING_PENDING;
}
