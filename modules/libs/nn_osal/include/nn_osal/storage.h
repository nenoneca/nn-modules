/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_osal/storage.h — key-value storage + DFU image staging.
 *
 * Two unrelated concerns share this header because they both wrap
 * non-volatile storage on the backend:
 *
 *   1. Key-value store: small persistent settings (~bytes to KB), keyed
 *      by forward-slash-namespaced strings.  Backed by Zephyr settings
 *      subsystem (on-device) or a flat file tree (on POSIX).
 *
 *   2. DFU image staging: writes blocks of a candidate firmware image
 *      into the secondary slot, then asks the bootloader to swap on
 *      next reboot.  Backed by Zephyr's flash_img + boot_request_upgrade
 *      (on-device); on POSIX this is a no-op so test fixtures can run.
 */

/* ── key-value ─────────────────────────────────────────────────────── */

typedef int (*nn_osal_kv_load_cb)(const char *key_suffix,
                                  const uint8_t *value, size_t value_len,
                                  void *user);

int nn_osal_kv_init(void);
int nn_osal_kv_register(const char *prefix,
                        nn_osal_kv_load_cb cb, void *user);
int nn_osal_kv_load_all(void);
int nn_osal_kv_save(const char *key, const void *data, size_t len);
int nn_osal_kv_delete(const char *key);

/* ── DFU ───────────────────────────────────────────────────────────── */

/* Opaque DFU context.  Allocate one of these with nn_osal_dfu_ctx_alloc()
 * — its internal size depends on the backend, so callers don't touch
 * the struct fields directly. */
typedef struct nn_osal_dfu_ctx nn_osal_dfu_ctx_t;

/* Returns a heap-allocated DFU context.  Caller frees with
 * nn_osal_dfu_ctx_free().  Sized for the active backend (Zephyr =
 * sizeof(struct flash_img_context)). */
nn_osal_dfu_ctx_t *nn_osal_dfu_ctx_alloc(void);
void               nn_osal_dfu_ctx_free(nn_osal_dfu_ctx_t *ctx);

/* Prepare for writing the secondary slot (erases it). */
int nn_osal_dfu_begin(nn_osal_dfu_ctx_t *ctx, size_t expected_size);

/* Write a contiguous chunk at the current offset. */
int nn_osal_dfu_write(nn_osal_dfu_ctx_t *ctx,
                      const uint8_t *buf, size_t len);

/* Finalise the image (validate header / SHA, mark slot bootable). */
int nn_osal_dfu_finalise(nn_osal_dfu_ctx_t *ctx);

/* Request the bootloader to swap on next reboot (test image).  The
 * second call after the new slot has booted confirms it; without
 * confirm the next reboot reverts. */
int nn_osal_dfu_request_upgrade(bool permanent);
int nn_osal_dfu_confirm(void);
