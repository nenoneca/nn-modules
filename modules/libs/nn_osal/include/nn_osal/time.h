/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

/*
 * nn_osal/time.h — monotonic time + sleep.
 *
 * Wall-clock / calendar time is intentionally NOT in the OSAL — that's
 * a network/protocol concern (see node_mgr/time_sync) and the answer
 * is platform-independent: the device just stores an epoch offset.
 */

/* Monotonic millisecond counter since boot.  Wraps at 2^63 ms (~292M
 * years); treat as never wrapping. */
int64_t nn_osal_uptime_ms(void);

/* 32-bit millisecond uptime (wraps every ~49 days).  Cheaper than the
 * 64-bit variant on some MCUs.  Use only for short timeouts. */
uint32_t nn_osal_uptime_ms_32(void);

/* Sleep the current thread.  Returns the number of ms still owed if
 * woken early (e.g. signal); 0 if it slept the full duration. */
uint32_t nn_osal_sleep_ms(uint32_t ms);

/* Microsecond busy-wait (no scheduling).  For very short delays only —
 * blocks the CPU. */
void nn_osal_busy_wait_us(uint32_t us);
