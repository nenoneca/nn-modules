/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal time backend — ESP-IDF (esp_timer + FreeRTOS delay). */
#include "nn_osal/time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

int64_t nn_osal_uptime_ms(void)     { return esp_timer_get_time() / 1000; }
uint32_t nn_osal_uptime_ms_32(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

uint32_t nn_osal_sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
    return 0;
}

void nn_osal_busy_wait_us(uint32_t us) { esp_rom_delay_us(us); }
