/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_pal/log_sink.h — runtime log-backend registration.
 *
 * Lets libs siphon formatted log messages out of the platform logger
 * without including <zephyr/logging/log_backend*.h> directly.  The PAL
 * backend (Zephyr) registers itself with the platform's log subsystem;
 * the lib registers a callback that receives processed log messages.
 *
 * Typical use: forward LOG_INF / LOG_WRN / LOG_ERR records to the hub
 * as D2H LOG_LINE frames.  See node_mgr/coap_log.c.
 *
 * Single sink per build — late registrations replace earlier ones.
 *
 * The Zephyr backend exposes this on top of LOG_BACKEND_DEFINE + a
 * log_output_func.  Other backends (POSIX, RTT-only, ...) may stub
 * register/unregister with -ENOSYS.
 */

/* Severity classification for filtering. */
typedef enum {
    NN_PAL_LOG_LEVEL_ERR  = 1,
    NN_PAL_LOG_LEVEL_WRN  = 2,
    NN_PAL_LOG_LEVEL_INF  = 3,
    NN_PAL_LOG_LEVEL_DBG  = 4,
} nn_pal_log_level_t;

/* Callback receives one fully-formatted log line per call.  `data`
 * is the formatted text (no trailing newline guaranteed); `len` is
 * the byte count.  Caller MUST NOT block the log thread for long. */
typedef void (*nn_pal_log_sink_cb_t)(uint8_t level,
                                     const uint8_t *data, size_t len,
                                     void *user);

/* Register a sink + activate it at the given minimum severity.  Only
 * messages at or above min_level are delivered.  Idempotent: re-
 * registering with a new cb replaces the previous one.  Returns 0 on
 * success, negative errno otherwise. */
int nn_pal_log_sink_register(nn_pal_log_sink_cb_t cb, void *user,
                             uint8_t min_level);

/* Disable + deregister the sink. */
int nn_pal_log_sink_unregister(void);

/* Called from a panic / hard-fault path — sink will receive no
 * further messages after this.  Equivalent to log_backend_panic on
 * Zephyr.  Safe to call from interrupt context. */
void nn_pal_log_sink_panic(void);

/* Suspend / resume the platform's log-processing thread.  Used by
 * callers (e.g. ota_client during flash erase/write) that must guard
 * against deferred log processing colliding with low-level hardware
 * operations.  No-op if the platform's logger is synchronous. */
void nn_pal_log_thread_suspend(void);
void nn_pal_log_thread_resume(void);
