/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

/*
 * nn_osal/system.h — system-level control: reboot, init order.
 *
 * Reboot type:
 *   COLD     — full system reset (cache + MMU + RAM reload).  This is
 *              the only safe reboot during firmware OTA on ESP32-C6;
 *              see the MCUboot warm-reset memory note.
 *   WARM     — CPU-only reset.  Cheaper but on some SoCs leaves cache
 *              state stale; use only when you know the platform.
 *
 * Init hooks:
 *   NN_OSAL_INIT(fn, level)
 *     Run `fn` at boot, ordered by level (0 = earliest, 99 = latest).
 *     On Zephyr maps to SYS_INIT(fn, APPLICATION, level).  Use sparingly
 *     — most modules should expose an explicit init() called from main.
 */

typedef enum {
    NN_OSAL_REBOOT_COLD = 0,
    NN_OSAL_REBOOT_WARM = 1,
} nn_osal_reboot_type_t;

void nn_osal_sys_reboot(nn_osal_reboot_type_t type) __attribute__((noreturn));

/* Halt the system.  Used by panic / hard-fault.  Backend-specific:
 * Zephyr -> k_oops; POSIX -> abort(). */
void nn_osal_sys_panic(const char *reason) __attribute__((noreturn));

/* Compile-time init hook.  Used by modules that must register before
 * main() runs.  level is opaque-ish but follows Zephyr's convention:
 *   <40 = pre-kernel-2,  40-79 = post-kernel,  80+ = application. */
#define NN_OSAL_INIT(fn, level) _NN_OSAL_INIT_IMPL(fn, level)

/* Earlier init hook — runs before drivers + L2 init.  Use when you
 * must execute before a Zephyr driver's POST_KERNEL init priority (or
 * the equivalent driver-bring-up phase on other backends).  Required
 * for nn project's BLE coex tuning that must beat the 802.15.4
 * driver init at priority 80 (see network_manager.c). */
#define NN_OSAL_INIT_EARLY(fn, level) _NN_OSAL_INIT_EARLY_IMPL(fn, level)

/* App version string (e.g. "2.4.0+0") set at build time by the
 * factory script.  Returns the tweak-aware form ("2.4.0+0") when
 * available, falling back to the bare semver ("2.4.0") otherwise.
 * Returns NULL if neither was provided at build time. */
const char *nn_osal_app_version(void);

#include "internal/backend_system.h"
