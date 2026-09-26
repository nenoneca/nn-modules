/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/time.h>
#include <zephyr/kernel.h>

int64_t  nn_osal_uptime_ms(void)      { return k_uptime_get(); }
uint32_t nn_osal_uptime_ms_32(void)   { return k_uptime_get_32(); }

uint32_t nn_osal_sleep_ms(uint32_t ms) { return k_msleep(ms); }

void nn_osal_busy_wait_us(uint32_t us) { k_busy_wait(us); }
