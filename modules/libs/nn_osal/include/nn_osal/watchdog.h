/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

/*
 * nn_osal/watchdog.h — task watchdog (kernel-level).
 *
 * The nn project uses Zephyr's `task_wdt` API (a software watchdog
 * built on top of a hardware WDT) rather than the raw driver-level
 * `wdt_*` API.  Reasons:
 *   - Hardware WDT semantics (period, alert/reset behaviour, multi-
 *     channel config) vary wildly across vendors.  task_wdt picks one
 *     hardware WDT and multiplexes virtual channels on top.
 *   - Each thread that wants WDT protection registers its own channel
 *     with its own per-channel timeout.  A starved channel → callback,
 *     then SoC reset.
 *
 * On non-Zephyr backends, a port would either wrap an equivalent
 * software-WDT layer (FreeRTOS' optional WDT helpers, etc.) or
 * synthesise one over a kernel timer + raw HW WDT.
 */

typedef int nn_osal_wdt_channel_t;  /* >=0 valid; <0 error */

typedef void (*nn_osal_wdt_panic_cb_t)(nn_osal_wdt_channel_t channel,
                                       void *user);

/* Initialise the task watchdog.  Called once at boot, before any
 * task_wdt_add().  `panic_cb` fires synchronously from the WDT thread
 * when ANY channel expires; it should log + then return (the runtime
 * forces a reset).  Pass NULL for default behaviour. */
int nn_osal_task_wdt_init(uint32_t hardware_period_ms,
                          nn_osal_wdt_panic_cb_t panic_cb,
                          void *user);

/* Register a virtual channel.  Returns >=0 channel id on success.
 * `timeout_ms` is the per-channel grace period; if it elapses without
 * a feed(), the panic_cb fires. */
nn_osal_wdt_channel_t nn_osal_task_wdt_add(uint32_t timeout_ms);

/* Kick the channel.  Call periodically — at least once per
 * `timeout_ms`. */
int nn_osal_task_wdt_feed(nn_osal_wdt_channel_t channel);

#include "internal/backend_watchdog.h"
