/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal GPIO surface — POSIX backend.
 * Hosted Linux has no flat pin space; pins live in the same named registry
 * the other backends use, with in-memory logical state.  This is enough for
 * every current caller (nn_link handshake line, nn_p4ctl boot/reset pins are
 * ESP-only paths); real hardware (libgpiod on the BeagleY-AI) can replace the
 * level accessors behind the same struct without touching callers. */
#include <nn_osal/gpio.h>
#include <string.h>
#include <errno.h>

#define NN_OSAL_GPIO_REG_MAX  16

static struct { const char *name; nn_osal_gpio_pin_t *pin; } s_reg[NN_OSAL_GPIO_REG_MAX];
static unsigned s_reg_count;

nn_osal_gpio_pin_t *nn_osal_gpio_get(const char *name)
{
    if (!name) return NULL;
    for (unsigned i = 0; i < s_reg_count; i++)
        if (s_reg[i].name && strcmp(s_reg[i].name, name) == 0)
            return s_reg[i].pin;
    return NULL;
}

int nn_osal_gpio_register(const char *name, nn_osal_gpio_pin_t *pin)
{
    if (!name || !pin || s_reg_count >= NN_OSAL_GPIO_REG_MAX) return -ENOMEM;
    s_reg[s_reg_count].name = name;
    s_reg[s_reg_count].pin  = pin;
    s_reg_count++;
    return 0;
}

int nn_osal_gpio_from_raw(nn_osal_gpio_pin_t *pin, nn_osal_gpio_port_t *port,
                          uint32_t pin_no, uint32_t flags)
{
    pin->port  = (void *)port;
    pin->pin   = pin_no;
    pin->flags = flags;
    pin->level = 0;
    return 0;
}

int nn_osal_gpio_configure(nn_osal_gpio_pin_t *pin, uint32_t flags)
{
    pin->flags = flags;
    return 0;
}

int nn_osal_gpio_get_logical(const nn_osal_gpio_pin_t *pin) { return pin->level; }

int nn_osal_gpio_set_logical(nn_osal_gpio_pin_t *pin, int value)
{
    pin->level = value ? 1 : 0;
    return 0;
}

int nn_osal_gpio_toggle(nn_osal_gpio_pin_t *pin)
{
    pin->level = !pin->level;
    return 0;
}

bool nn_osal_gpio_port_ready(nn_osal_gpio_port_t *port) { (void)port; return true; }

void nn_osal_gpio_callback_init(nn_osal_gpio_callback_t *cb,
                                nn_osal_gpio_cb_t handler, uint32_t pin_mask)
{
    cb->user_handler = handler;
    cb->pin_mask     = pin_mask;
}

int nn_osal_gpio_add_callback(nn_osal_gpio_pin_t *pin, nn_osal_gpio_callback_t *cb)
{
    (void)pin; (void)cb;
    return -ENOSYS;    /* interrupt delivery needs real hardware (libgpiod) */
}

int nn_osal_gpio_interrupt_configure(nn_osal_gpio_pin_t *pin, uint32_t mode)
{
    (void)pin; (void)mode;
    return -ENOSYS;
}
