/* SPDX-License-Identifier: Apache-2.0 */
#include "nn_p4ctl/nn_p4ctl.h"
#include <nn_osal/gpio.h>
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_p4ctl);
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#define RESET_HOLD_MS    30     /* how long to hold the P4 in reset */
#define STRAP_SETTLE_MS  20     /* let BOOT strap settle before release */

static bool s_inited;
/* BOOT/RESET are plain active-high control outputs (no bus role), so they map
 * cleanly onto the nn_osal GPIO surface. */
static nn_osal_gpio_pin_t s_boot;
static nn_osal_gpio_pin_t s_reset;

esp_err_t nn_p4ctl_init(void)
{
    if (s_inited) return ESP_OK;

    nn_osal_gpio_from_raw(&s_boot,  NULL, NN_P4CTL_PIN_BOOT,  0);
    nn_osal_gpio_from_raw(&s_reset, NULL, NN_P4CTL_PIN_RESET, 0);
    /* Idle: P4 running (RESET high), normal boot (BOOT high). */
    if (nn_osal_gpio_configure(&s_boot,  NN_OSAL_GPIO_OUTPUT | NN_OSAL_GPIO_INIT_HIGH) ||
        nn_osal_gpio_configure(&s_reset, NN_OSAL_GPIO_OUTPUT | NN_OSAL_GPIO_INIT_HIGH)) {
        NN_LOG_ERR("gpio configure failed");
        return ESP_FAIL;
    }
    s_inited = true;
    NN_LOG_INF("init (BOOT=GPIO%d, RESET=GPIO%d)",
             NN_P4CTL_PIN_BOOT, NN_P4CTL_PIN_RESET);
    return ESP_OK;
}

esp_err_t nn_p4ctl_assert_reset(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    nn_osal_gpio_set_logical(&s_reset, 0);
    return ESP_OK;
}

esp_err_t nn_p4ctl_release_reset(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    nn_osal_gpio_set_logical(&s_reset, 1);
    return ESP_OK;
}

esp_err_t nn_p4ctl_power_cycle(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    NN_LOG_INF("power-cycle P4 (normal boot)");
    nn_osal_gpio_set_logical(&s_boot, 1);     /* normal-boot strap */
    nn_osal_gpio_set_logical(&s_reset, 0);    /* hold in reset      */
    vTaskDelay(pdMS_TO_TICKS(RESET_HOLD_MS));
    nn_osal_gpio_set_logical(&s_reset, 1);    /* release            */
    return ESP_OK;
}

esp_err_t nn_p4ctl_enter_download(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    NN_LOG_INF("reboot P4 into ROM download mode");
    nn_osal_gpio_set_logical(&s_boot, 0);     /* download strap low */
    nn_osal_gpio_set_logical(&s_reset, 0);    /* hold in reset      */
    vTaskDelay(pdMS_TO_TICKS(RESET_HOLD_MS));
    nn_osal_gpio_set_logical(&s_reset, 1);    /* release while low  */
    vTaskDelay(pdMS_TO_TICKS(STRAP_SETTLE_MS));
    nn_osal_gpio_set_logical(&s_boot, 1);     /* release strap      */
    return ESP_OK;
}
