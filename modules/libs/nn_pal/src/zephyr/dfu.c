/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_pal/dfu.h>

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <nn_osal/osal.h>

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/storage/flash_map.h>

NN_OSAL_LOG_MODULE(nn_pal_dfu);

/* The opaque PAL context wraps Zephyr's flash_img_context.  We use a
 * single static instance (one DFU at a time on this hardware) and hand
 * out a pointer to it from alloc — the caller's "ctx" is purely a
 * non-NULL handle. */

struct nn_pal_dfu_ctx {
    struct flash_img_context inner;
    bool                     active;
};

static struct nn_pal_dfu_ctx s_ctx;

nn_pal_dfu_ctx_t *nn_pal_dfu_ctx_alloc(void)
{
    if (s_ctx.active) {
        NN_LOG_WRN("ctx_alloc while previous still active");
        return NULL;
    }
    memset(&s_ctx, 0, sizeof s_ctx);
    s_ctx.active = true;
    return &s_ctx;
}

void nn_pal_dfu_ctx_free(nn_pal_dfu_ctx_t *ctx)
{
    if (ctx) ctx->active = false;
}

int nn_pal_dfu_begin(nn_pal_dfu_ctx_t *ctx, size_t expected_size)
{
    (void)expected_size;
    if (!ctx) return -EINVAL;
    int rv = flash_img_init(&ctx->inner);
    if (rv) NN_LOG_ERR("flash_img_init: %d", rv);
    return rv;
}

int nn_pal_dfu_write(nn_pal_dfu_ctx_t *ctx, const uint8_t *buf, size_t len)
{
    if (!ctx || !buf) return -EINVAL;
    /* The Zephyr helper handles flush implicitly when a chunk crosses
     * a write-block boundary; final-flush happens at finalise. */
    return flash_img_buffered_write(&ctx->inner, (uint8_t *)buf, len, false);
}

int nn_pal_dfu_finalise(nn_pal_dfu_ctx_t *ctx)
{
    if (!ctx) return -EINVAL;
    /* `true` here forces the final buffered chunk out + closes the
     * flash area. */
    return flash_img_buffered_write(&ctx->inner, NULL, 0, true);
}

int nn_pal_dfu_request_upgrade(bool permanent)
{
    return boot_request_upgrade(permanent ? BOOT_UPGRADE_PERMANENT
                                          : BOOT_UPGRADE_TEST);
}

int nn_pal_dfu_confirm(void)
{
    return boot_write_img_confirmed();
}

int nn_pal_dfu_get_state(nn_pal_dfu_state_t *out)
{
    if (!out) return -EINVAL;

    bool pending = !boot_is_img_confirmed();
    /* boot_request_upgrade hasn't been called from here; we can't
     * cleanly tell STAGED apart from RUNNING_CONFIRMED without poking
     * the trailer of the secondary slot directly.  For now report
     * pending vs confirmed; STAGED is left for future expansion. */
    *out = pending ? NN_PAL_DFU_STATE_RUNNING_PENDING
                   : NN_PAL_DFU_STATE_RUNNING_CONFIRMED;
    return 0;
}

int nn_pal_dfu_erase_secondary(void)
{
    /* boot_erase_img_bank takes the flash-area id of the secondary
     * slot; on this project that's FIXED_PARTITION_ID(slot1_partition). */
    return boot_erase_img_bank(FIXED_PARTITION_ID(slot1_partition));
}

/* ── Random-access slot I/O (chunk-diff OTA) ──────────────────────────── */

static int slot_fa_id(int slot)
{
    switch (slot) {
    case NN_PAL_DFU_SLOT_PRIMARY:   return FIXED_PARTITION_ID(slot0_partition);
    case NN_PAL_DFU_SLOT_SECONDARY: return FIXED_PARTITION_ID(slot1_partition);
    default:                        return -1;
    }
}

int nn_pal_dfu_slot_read(int slot, uint32_t off, uint8_t *buf, size_t len)
{
    if (!buf) return -EINVAL;
    int fa_id = slot_fa_id(slot);
    if (fa_id < 0) return -EINVAL;

    const struct flash_area *fa;
    int rv = flash_area_open(fa_id, &fa);
    if (rv) return rv;
    rv = flash_area_read(fa, off, buf, len);
    flash_area_close(fa);
    return rv;
}

int nn_pal_dfu_slot1_erase(uint32_t off, size_t len)
{
    if ((off | len) & (NN_PAL_DFU_SECTOR_SIZE - 1)) return -EINVAL;

    const struct flash_area *fa;
    int rv = flash_area_open(FIXED_PARTITION_ID(slot1_partition), &fa);
    if (rv) return rv;
    rv = flash_area_erase(fa, off, len);
    flash_area_close(fa);
    return rv;
}

int nn_pal_dfu_slot1_write(uint32_t off, const uint8_t *buf, size_t len)
{
    if (!buf) return -EINVAL;

    const struct flash_area *fa;
    int rv = flash_area_open(FIXED_PARTITION_ID(slot1_partition), &fa);
    if (rv) return rv;
    rv = flash_area_write(fa, off, buf, len);
    flash_area_close(fa);
    return rv;
}

uint32_t nn_pal_dfu_slot1_size(void)
{
    const struct flash_area *fa;
    if (flash_area_open(FIXED_PARTITION_ID(slot1_partition), &fa)) {
        return 0;
    }
    uint32_t sz = (uint32_t)fa->fa_size;
    flash_area_close(fa);
    return sz;
}
