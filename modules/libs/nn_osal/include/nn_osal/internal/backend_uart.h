/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#ifdef CONFIG_NN_OSAL_BACKEND_ZEPHYR
#  include <zephyr/drivers/uart.h>
#  include <zephyr/device.h>

struct nn_osal_uart {
    const struct device  *dev;
    nn_osal_uart_irq_cb_t user_cb;
    void                 *user_data;
};
#elif defined(CONFIG_NN_OSAL_BACKEND_AMEBA)
#  include <stdint.h>
/* The real implementation binds to the SDK's serial_t (component/mbed/hal).
 * Kept opaque here so this header does not drag the whole HAL into every
 * translation unit that includes nn_osal/uart.h. */
struct nn_osal_uart {
    void                 *dev;    /* serial_t * */
    nn_osal_uart_irq_cb_t user_cb;
    void                 *user_data;
};
#else
#  error "no nn_osal backend selected (ZEPHYR / ESP_IDF / POSIX / AMEBA)"
#endif
