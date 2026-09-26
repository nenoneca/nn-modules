/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/*
 * Storage backend defines its own opaque struct sizes here.
 *
 * NB: we DON'T include <zephyr/dfu/flash_img.h> here because that
 * header transitively requires CONFIG_IMG_BLOCK_BUF_SIZE which only
 * exists when CONFIG_IMG_BLOCK_BUF=y.  Apps that link the OSAL but
 * don't enable IMG_BLOCK_BUF would otherwise fail at preprocess.
 *
 * The actual struct shape lives in src/zephyr/storage.c (heap-allocated
 * inside nn_osal_dfu_ctx_alloc()) so callers never see flash_img_context.
 */

#ifdef CONFIG_NN_OSAL_BACKEND_ZEPHYR
/* No public types needed — DFU ctx is heap-allocated and opaque. */
#elif defined(CONFIG_NN_OSAL_BACKEND_ESP_IDF)
/* No public types needed — KV is NVS-backed, DFU ctx is heap-allocated and
 * opaque (src/esp_idf/storage.c). */
#elif defined(CONFIG_NN_OSAL_BACKEND_POSIX)
/* No public types needed — KV is a directory of files (NN_OSAL_KV_DIR, default
 * /var/lib/nn/kv), DFU ctx is heap-allocated and opaque. */
#elif defined(CONFIG_NN_OSAL_BACKEND_AMEBA)
/* No public types needed.  KV will be flash-backed (the SDK's KV/flash API
 * over the data partition); DFU ctx stays heap-allocated and opaque.
 *
 * Whatever backs it MUST implement register-then-load and survive a restart:
 * only the Zephyr backend loads on register, so posix and esp_idf need
 * load_all, and T1's kv case re-execs a fresh process precisely so in-process
 * statics cannot hide a dead load path. */
#else
#  error "no nn_osal backend selected (ZEPHYR / ESP_IDF / POSIX / AMEBA)"
#endif
