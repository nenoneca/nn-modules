/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdarg.h>
#include <stdint.h>

/*
 * nn_osal/log.h — printf-style logging.
 *
 * The OSAL exposes a tiny printf-style API.  The backend decides where
 * the output goes (Zephyr LOG subsystem on-device; stderr on POSIX).
 *
 * Convention: each module declares a tag at file scope via
 *   NN_OSAL_LOG_MODULE("my_mod");
 * and then calls NN_LOG_INF / WRN / ERR / DBG.  The tag becomes the
 * "module" name on Zephyr; on POSIX it's prepended to each line.
 *
 * Levels are clamped at compile time by the active backend (Zephyr
 * lifts CONFIG_NN_OSAL_LOG_LEVEL into CONFIG_LOG_MODULE_OVERRIDE_LEVEL).
 */

typedef enum {
    NN_OSAL_LOG_OFF  = 0,
    NN_OSAL_LOG_ERR  = 1,
    NN_OSAL_LOG_WRN  = 2,
    NN_OSAL_LOG_INF  = 3,
    NN_OSAL_LOG_DBG  = 4,
} nn_osal_log_level_t;

/* Compile-time module declaration (uses backend-specific macros). */
#define NN_OSAL_LOG_MODULE(name) _NN_OSAL_LOG_MODULE_IMPL(name)

/* Core call.  Use the helpers below in client code. */
void nn_osal_log_emit(nn_osal_log_level_t level,
                      const char *module, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void nn_osal_log_emit_v(nn_osal_log_level_t level,
                        const char *module, const char *fmt, va_list ap);

/* Always-printk fallback — bypasses level filtering, used by panic /
 * pre-init paths. */
void nn_osal_printk(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* Public per-level helpers.  These resolve to a backend-native macro
 * so on Zephyr they preserve the LOG_INF / LOG_WRN compile-time
 * filtering and runtime module-level overrides. */
#include "internal/backend_log.h"

#ifndef NN_LOG_INF
#define NN_LOG_INF(fmt, ...) nn_osal_log_emit(NN_OSAL_LOG_INF, _NN_OSAL_LOG_TAG, fmt, ##__VA_ARGS__)
#endif
#ifndef NN_LOG_WRN
#define NN_LOG_WRN(fmt, ...) nn_osal_log_emit(NN_OSAL_LOG_WRN, _NN_OSAL_LOG_TAG, fmt, ##__VA_ARGS__)
#endif
#ifndef NN_LOG_ERR
#define NN_LOG_ERR(fmt, ...) nn_osal_log_emit(NN_OSAL_LOG_ERR, _NN_OSAL_LOG_TAG, fmt, ##__VA_ARGS__)
#endif
#ifndef NN_LOG_DBG
#define NN_LOG_DBG(fmt, ...) nn_osal_log_emit(NN_OSAL_LOG_DBG, _NN_OSAL_LOG_TAG, fmt, ##__VA_ARGS__)
#endif
