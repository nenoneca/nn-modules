/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * nn_ota_dfu — esp_ota / esp_partition-backed staging into the INACTIVE OTA
 * slot.  This is the media equivalent of the sensors' MCUboot nn_pal_dfu_*:
 * the same download->stage->verify->swap flow, backed by esp_partition writes
 * (sector-addressable, erase-on-first-touch, so resumed/out-of-order sector
 * writes work) + esp_ota_set_boot_partition for the swap.
 *
 * Used by BOTH chips: the C6 stages its own image; the P4 stages the image the
 * C6 streams to it over nn_link.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Begin staging an image of `total_size` bytes into the next (inactive) slot.
 * Records the target partition and resets the sector buffer.  Does NOT erase
 * the whole slot up-front (sectors are erased on first write, for resume). */
esp_err_t nn_ota_dfu_begin(size_t total_size);

/* Write `len` bytes at byte `offset` of the staged image.  Buffers a 4 KiB
 * sector in RAM and flushes (erase+write) when the offset crosses into the
 * next sector.  Offsets within a staging pass must be non-decreasing. */
esp_err_t nn_ota_dfu_write(size_t offset, const void *data, size_t len);

/* Flush any buffered partial sector to flash. */
esp_err_t nn_ota_dfu_flush(void);

/* SHA-256 over the first `size` bytes of the staged slot (flushes first). */
esp_err_t nn_ota_dfu_sha256(size_t size, uint8_t out[32]);

/* Select the staged slot as the next boot partition (boots in TEST mode when
 * rollback is enabled, so it must be confirmed or it reverts). */
esp_err_t nn_ota_dfu_set_boot(void);

/* Mark the CURRENTLY RUNNING app valid (cancel a pending rollback). */
esp_err_t nn_ota_dfu_mark_valid(void);

/* Copy the running app version string (from esp_app_get_description). */
void nn_ota_dfu_running_version(char *buf, size_t n);

/* True if the running app booted pending-verify (i.e. just OTA-swapped and not
 * yet confirmed). */
bool nn_ota_dfu_pending_verify(void);

#ifdef __cplusplus
}
#endif
