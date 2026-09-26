/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/uart.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <errno.h>

/* ── named registry ─────────────────────────────────────────────── */

#define NN_OSAL_UART_REG_MAX  4

struct uart_reg_entry {
    const char     *name;
    nn_osal_uart_t *uart;
};
static struct uart_reg_entry s_reg[NN_OSAL_UART_REG_MAX];
static unsigned              s_reg_count;

nn_osal_uart_t *nn_osal_uart_get(const char *name)
{
    if (!name) return NULL;
    for (unsigned i = 0; i < s_reg_count; i++) {
        if (s_reg[i].name && strcmp(s_reg[i].name, name) == 0) {
            return s_reg[i].uart;
        }
    }
    return NULL;
}

int nn_osal_uart_register(const char *name, nn_osal_uart_t *uart)
{
    if (!name || !uart) return -EINVAL;
    if (s_reg_count >= NN_OSAL_UART_REG_MAX) return -ENOMEM;
    for (unsigned i = 0; i < s_reg_count; i++) {
        if (s_reg[i].name && strcmp(s_reg[i].name, name) == 0) {
            s_reg[i].uart = uart;
            return 0;
        }
    }
    s_reg[s_reg_count].name = name;
    s_reg[s_reg_count].uart = uart;
    s_reg_count++;
    return 0;
}

bool nn_osal_uart_is_ready(const nn_osal_uart_t *uart)
{
    if (!uart || !uart->dev) return false;
    return device_is_ready(uart->dev);
}

/* ── IRQ-driven RX ──────────────────────────────────────────────── */

/* Zephyr's uart_irq_callback wants `void cb(const struct device *,
 * void *user)`.  We stash a pointer to nn_osal_uart_t in user_data and
 * forward through the user's nn_osal_uart_irq_cb_t. */
static void irq_trampoline(const struct device *dev, void *user_data)
{
    (void)dev;
    nn_osal_uart_t *u = (nn_osal_uart_t *)user_data;
    if (u && u->user_cb) {
        u->user_cb(u, u->user_data);
    }
}

void nn_osal_uart_irq_callback_set(nn_osal_uart_t *u,
                                   nn_osal_uart_irq_cb_t cb,
                                   void *user)
{
    if (!u || !u->dev) return;
    u->user_cb   = cb;
    u->user_data = user;
    uart_irq_callback_user_data_set(u->dev, irq_trampoline, u);
}

void nn_osal_uart_irq_rx_enable(nn_osal_uart_t *u)
{
    if (u && u->dev) uart_irq_rx_enable(u->dev);
}

void nn_osal_uart_irq_rx_disable(nn_osal_uart_t *u)
{
    if (u && u->dev) uart_irq_rx_disable(u->dev);
}

bool nn_osal_uart_irq_is_pending(nn_osal_uart_t *u)
{
    if (!u || !u->dev) return false;
    return uart_irq_is_pending(u->dev) != 0;
}

int nn_osal_uart_irq_update(nn_osal_uart_t *u)
{
    if (!u || !u->dev) return -EINVAL;
    return uart_irq_update(u->dev);
}

bool nn_osal_uart_irq_rx_ready(nn_osal_uart_t *u)
{
    if (!u || !u->dev) return false;
    return uart_irq_rx_ready(u->dev) != 0;
}

int nn_osal_uart_fifo_read(nn_osal_uart_t *u, uint8_t *buf, size_t len)
{
    if (!u || !u->dev || !buf) return -EINVAL;
    return uart_fifo_read(u->dev, buf, len);
}

void nn_osal_uart_poll_out(nn_osal_uart_t *u, uint8_t byte)
{
    if (u && u->dev) uart_poll_out(u->dev, byte);
}
