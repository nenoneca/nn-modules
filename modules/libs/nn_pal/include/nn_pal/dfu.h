/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_pal/dfu.h — Firmware DFU.
 *
 * Promotes `nn_osal/storage.h`'s DFU section (P1) into its own PAL
 * surface, then extends it to cover the semantic delta between Zephyr's
 * slot+trailer (MCUboot) model and ESP-IDF's partition+set-boot model.
 *
 * The API stays MCUboot-shaped (begin / write / finalise / request_upgrade
 * / confirm) because that's the model the nn project's OTA flow assumes.
 * ESP-IDF backends translate by mapping:
 *   - begin/write → esp_ota_begin + esp_ota_write
 *   - finalise   → esp_ota_end + esp_ota_set_boot_partition(NEW)
 *   - confirm    → esp_ota_mark_app_valid_cancel_rollback()
 *   - request_upgrade(TEST) → set_boot_partition without mark_valid
 *     (next boot tries new; if app doesn't call confirm, watchdog
 *     reboot rolls back via esp_ota_check_rollback_is_possible)
 *
 * Boot state introspection (nn_pal_dfu_get_state) lets callers
 * detect "just-swapped, not confirmed yet" vs "running confirmed image"
 * vs "image marked bad" — useful for the post-OTA `boot_write_img_confirmed`
 * dance.
 */

typedef struct nn_pal_dfu_ctx nn_pal_dfu_ctx_t;

typedef enum {
    /* App is running confirmed; no pending swap. */
    NN_PAL_DFU_STATE_RUNNING_CONFIRMED  = 0,
    /* App just booted from a freshly-swapped slot, waiting for confirm.
     * If we reboot without calling nn_pal_dfu_confirm() the loader
     * reverts. */
    NN_PAL_DFU_STATE_RUNNING_PENDING    = 1,
    /* Secondary slot holds a downloaded image awaiting reboot to apply. */
    NN_PAL_DFU_STATE_STAGED             = 2,
    /* No DFU info available (no MCUboot / non-ota build). */
    NN_PAL_DFU_STATE_UNKNOWN            = 99,
} nn_pal_dfu_state_t;

/* Allocate a DFU context (opaque size — backend decides). */
nn_pal_dfu_ctx_t *nn_pal_dfu_ctx_alloc(void);
void              nn_pal_dfu_ctx_free(nn_pal_dfu_ctx_t *ctx);

/* Prepare for writing the secondary slot (erases it). */
int nn_pal_dfu_begin(nn_pal_dfu_ctx_t *ctx, size_t expected_size);

/* Write a contiguous chunk at the current offset. */
int nn_pal_dfu_write(nn_pal_dfu_ctx_t *ctx,
                     const uint8_t *buf, size_t len);

/* Finalise the write (validate header / SHA / signature). */
int nn_pal_dfu_finalise(nn_pal_dfu_ctx_t *ctx);

/* Request that the bootloader swap-or-set the new image on next reboot.
 * `permanent=false` (TEST): swap once; if the app doesn't confirm,
 * revert on the boot after that.
 * `permanent=true`: confirm in one step (no rollback safety net). */
int nn_pal_dfu_request_upgrade(bool permanent);

/* Confirm the currently-running image as good.  Call after a successful
 * boot of a pending image to prevent the loader from reverting on next
 * reboot. */
int nn_pal_dfu_confirm(void);

/* Read the current DFU lifecycle state.  Returns 0 on success and
 * writes to *out. */
int nn_pal_dfu_get_state(nn_pal_dfu_state_t *out);

/* Erase the secondary slot (idempotent — safe to call on a partially-
 * written or empty slot). */
int nn_pal_dfu_erase_secondary(void);

/* ── Random-access slot I/O (chunk-diff OTA) ────────────────────────────
 *
 * The chunk-diff download avoids re-fetching blocks the device already
 * holds: it reads the secondary slot (leftovers from an interrupted or
 * previous run) and the primary slot (the running image, large parts of
 * which are byte-identical across adjacent versions), and only
 * downloads sectors that differ from the target checksums.
 *
 * Sector granularity: flash erase is 4 KiB on the supported parts;
 * writes must target freshly-erased ranges.  Reads have no alignment
 * requirements beyond what the flash driver imposes (byte-granular on
 * Zephyr flash_area_read).  Reading the primary slot while executing
 * from it is safe — reads go through the flash driver, not XIP.
 */

#define NN_PAL_DFU_SLOT_PRIMARY    0
#define NN_PAL_DFU_SLOT_SECONDARY  1

#define NN_PAL_DFU_SECTOR_SIZE     4096u

/* Read `len` bytes at `off` from the given slot. */
int nn_pal_dfu_slot_read(int slot, uint32_t off, uint8_t *buf, size_t len);

/* Erase `len` bytes at `off` in the SECONDARY slot.  Both must be
 * sector-aligned (NN_PAL_DFU_SECTOR_SIZE). */
int nn_pal_dfu_slot1_erase(uint32_t off, size_t len);

/* Write `len` bytes at `off` into the SECONDARY slot.  The range must
 * have been erased first.  `off` and `len` must satisfy the flash
 * write-block alignment (4 B on the supported parts). */
int nn_pal_dfu_slot1_write(uint32_t off, const uint8_t *buf, size_t len);

/* Total size of the SECONDARY slot in bytes (0 on error).  Used by the
 * delta-OTA path to stage the patch in the slot's tail (above the
 * reconstructed image) without a dedicated partition. */
uint32_t nn_pal_dfu_slot1_size(void);
