/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal time backend — POSIX (CLOCK_MONOTONIC). */
#include "nn_osal/time.h"
#include <time.h>

int64_t nn_osal_uptime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint32_t nn_osal_uptime_ms_32(void) { return (uint32_t)nn_osal_uptime_ms(); }

uint32_t nn_osal_sleep_ms(uint32_t ms)
{
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0) { }   /* resume across signals */
    return 0;
}

void nn_osal_busy_wait_us(uint32_t us)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        clock_gettime(CLOCK_MONOTONIC, &t1);
    } while ((t1.tv_sec - t0.tv_sec) * 1000000L +
             (t1.tv_nsec - t0.tv_nsec) / 1000L < (long)us);
}
