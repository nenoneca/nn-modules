/* SPDX-License-Identifier: Apache-2.0 */
/* nn_pal/dfu.h — ESP-IDF backend.  Maps the MCUboot-shaped DFU surface onto
 * esp_ota / esp_partition:
 *   SLOT_PRIMARY   = the running partition
 *   SLOT_SECONDARY = esp_ota_get_next_update_partition() (the inactive OTA slot)
 *   slot1_*        = esp_partition_{erase_range,write,read} on the secondary
 *   request_upgrade= esp_ota_set_boot_partition (TEST: rollback armed)
 *   confirm        = esp_ota_mark_app_valid_cancel_rollback
 *   get_state      = esp_ota_get_state_partition(running)
 * This mirrors exactly the esp_ota calls nn_ota_dfu.c used before, so the
 * brick-risk OTA behaviour is unchanged — only the call site moved behind PAL. */
#if defined(CONFIG_NN_PAL_BACKEND_ESP_IDF)

#include <nn_pal/dfu.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <stdlib.h>
#include <errno.h>

struct nn_pal_dfu_ctx {
    esp_ota_handle_t handle;
    const esp_partition_t *target;
    bool open;
};

static const esp_partition_t *secondary(void)
{
    return esp_ota_get_next_update_partition(NULL);
}
static const esp_partition_t *primary(void)
{
    return esp_ota_get_running_partition();
}
static const esp_partition_t *slot_part(int slot)
{
    return slot == NN_PAL_DFU_SLOT_PRIMARY ? primary() : secondary();
}

/* ── streaming (ctx) API — esp_ota_begin/write/end ─────────────────────── */

nn_pal_dfu_ctx_t *nn_pal_dfu_ctx_alloc(void)
{
    return calloc(1, sizeof(struct nn_pal_dfu_ctx));
}
void nn_pal_dfu_ctx_free(nn_pal_dfu_ctx_t *ctx)
{
    if (ctx && ctx->open) esp_ota_abort(ctx->handle);
    free(ctx);
}

int nn_pal_dfu_begin(nn_pal_dfu_ctx_t *ctx, size_t expected_size)
{
    if (!ctx) return -EINVAL;
    ctx->target = secondary();
    if (!ctx->target) return -ENODEV;
    esp_err_t e = esp_ota_begin(ctx->target,
                                expected_size ? expected_size : OTA_SIZE_UNKNOWN,
                                &ctx->handle);
    if (e != ESP_OK) return -EIO;
    ctx->open = true;
    return 0;
}

int nn_pal_dfu_write(nn_pal_dfu_ctx_t *ctx, const uint8_t *buf, size_t len)
{
    if (!ctx || !ctx->open) return -EINVAL;
    return esp_ota_write(ctx->handle, buf, len) == ESP_OK ? 0 : -EIO;
}

int nn_pal_dfu_finalise(nn_pal_dfu_ctx_t *ctx)
{
    if (!ctx || !ctx->open) return -EINVAL;
    esp_err_t e = esp_ota_end(ctx->handle);
    ctx->open = false;
    return e == ESP_OK ? 0 : -EIO;
}

/* ── lifecycle ─────────────────────────────────────────────────────────── */

int nn_pal_dfu_request_upgrade(bool permanent)
{
    (void)permanent;   /* ESP-IDF confirms post-boot via nn_pal_dfu_confirm() */
    const esp_partition_t *s = secondary();
    if (!s) return -ENODEV;
    return esp_ota_set_boot_partition(s) == ESP_OK ? 0 : -EIO;
}

int nn_pal_dfu_confirm(void)
{
    return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK ? 0 : -EIO;
}

int nn_pal_dfu_get_state(nn_pal_dfu_state_t *out)
{
    if (!out) return -EINVAL;
    const esp_partition_t *run = primary();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) {
        *out = NN_PAL_DFU_STATE_UNKNOWN;
        return 0;
    }
    *out = (st == ESP_OTA_IMG_PENDING_VERIFY) ? NN_PAL_DFU_STATE_RUNNING_PENDING
                                              : NN_PAL_DFU_STATE_RUNNING_CONFIRMED;
    return 0;
}

int nn_pal_dfu_erase_secondary(void)
{
    const esp_partition_t *s = secondary();
    if (!s) return -ENODEV;
    return esp_partition_erase_range(s, 0, s->size) == ESP_OK ? 0 : -EIO;
}

/* ── random-access slot I/O (chunk-diff OTA) ───────────────────────────── */

int nn_pal_dfu_slot_read(int slot, uint32_t off, uint8_t *buf, size_t len)
{
    const esp_partition_t *p = slot_part(slot);
    if (!p) return -ENODEV;
    return esp_partition_read(p, off, buf, len) == ESP_OK ? 0 : -EIO;
}

int nn_pal_dfu_slot1_erase(uint32_t off, size_t len)
{
    const esp_partition_t *s = secondary();
    if (!s) return -ENODEV;
    return esp_partition_erase_range(s, off, len) == ESP_OK ? 0 : -EIO;
}

int nn_pal_dfu_slot1_write(uint32_t off, const uint8_t *buf, size_t len)
{
    const esp_partition_t *s = secondary();
    if (!s) return -ENODEV;
    return esp_partition_write(s, off, buf, len) == ESP_OK ? 0 : -EIO;
}

uint32_t nn_pal_dfu_slot1_size(void)
{
    const esp_partition_t *s = secondary();
    return s ? s->size : 0;
}

#endif /* CONFIG_NN_PAL_BACKEND_ESP_IDF */
