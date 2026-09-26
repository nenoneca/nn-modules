/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/gpio.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include <errno.h>

/* ── named-pin registry ─────────────────────────────────────────── */

#define NN_OSAL_GPIO_REG_MAX  16

struct gpio_reg_entry {
    const char         *name;
    nn_osal_gpio_pin_t *pin;
};
static struct gpio_reg_entry s_reg[NN_OSAL_GPIO_REG_MAX];
static unsigned              s_reg_count;

nn_osal_gpio_pin_t *nn_osal_gpio_get(const char *name)
{
    if (!name) return NULL;
    for (unsigned i = 0; i < s_reg_count; i++) {
        if (s_reg[i].name && strcmp(s_reg[i].name, name) == 0) {
            return s_reg[i].pin;
        }
    }
    return NULL;
}

int nn_osal_gpio_register(const char *name, nn_osal_gpio_pin_t *pin)
{
    if (!name || !pin) return -EINVAL;
    if (s_reg_count >= NN_OSAL_GPIO_REG_MAX) return -ENOMEM;
    /* Allow re-registration (overwrite) so a board can update at boot. */
    for (unsigned i = 0; i < s_reg_count; i++) {
        if (s_reg[i].name && strcmp(s_reg[i].name, name) == 0) {
            s_reg[i].pin = pin;
            return 0;
        }
    }
    s_reg[s_reg_count].name = name;
    s_reg[s_reg_count].pin  = pin;
    s_reg_count++;
    return 0;
}

int nn_osal_gpio_from_raw(nn_osal_gpio_pin_t *pin,
                          nn_osal_gpio_port_t *port,
                          uint32_t pin_number,
                          uint32_t flags)
{
    if (!pin || !port) return -EINVAL;
    pin->port  = (const struct device *)port;
    pin->pin   = pin_number;
    pin->flags = flags;
    return 0;
}

/* ── flag translation ───────────────────────────────────────────── */

static gpio_flags_t to_zflags(uint32_t f)
{
    gpio_flags_t z = 0;
    if (f & NN_OSAL_GPIO_INPUT)        z |= GPIO_INPUT;
    if (f & NN_OSAL_GPIO_OUTPUT)       z |= GPIO_OUTPUT;
    if (f & NN_OSAL_GPIO_PULL_UP)      z |= GPIO_PULL_UP;
    if (f & NN_OSAL_GPIO_PULL_DOWN)    z |= GPIO_PULL_DOWN;
    if (f & NN_OSAL_GPIO_ACTIVE_HIGH)  z |= GPIO_ACTIVE_HIGH;
    if (f & NN_OSAL_GPIO_ACTIVE_LOW)   z |= GPIO_ACTIVE_LOW;
    if (f & NN_OSAL_GPIO_INIT_HIGH)    z |= GPIO_OUTPUT_INIT_HIGH;
    if (f & NN_OSAL_GPIO_INIT_LOW)     z |= GPIO_OUTPUT_INIT_LOW;
    if (f & NN_OSAL_GPIO_OPEN_DRAIN)   z |= GPIO_OPEN_DRAIN;
    return z;
}

/* ── runtime API ────────────────────────────────────────────────── */

int nn_osal_gpio_configure(nn_osal_gpio_pin_t *p, uint32_t flags)
{
    if (!p || !p->port) return -EINVAL;
    p->flags = flags;
    return gpio_pin_configure(p->port, p->pin, to_zflags(flags));
}

int nn_osal_gpio_get_logical(const nn_osal_gpio_pin_t *p)
{
    if (!p || !p->port) return -EINVAL;
    return gpio_pin_get(p->port, p->pin);
}

int nn_osal_gpio_set_logical(nn_osal_gpio_pin_t *p, int value)
{
    if (!p || !p->port) return -EINVAL;
    return gpio_pin_set(p->port, p->pin, value);
}

int nn_osal_gpio_toggle(nn_osal_gpio_pin_t *p)
{
    if (!p || !p->port) return -EINVAL;
    return gpio_pin_toggle(p->port, p->pin);
}

/* ── interrupts ────────────────────────────────────────────────── */

static void cb_trampoline(const struct device *port,
                          struct gpio_callback *gcb,
                          gpio_port_pins_t pins)
{
    (void)port;
    nn_osal_gpio_callback_t *cb =
        CONTAINER_OF(gcb, nn_osal_gpio_callback_t, cb);
    if (cb->user_handler) {
        cb->user_handler(NULL, cb, pins);
    }
}

void nn_osal_gpio_callback_init(nn_osal_gpio_callback_t *cb,
                                nn_osal_gpio_cb_t handler,
                                uint32_t pin_mask)
{
    if (!cb || !handler) return;
    cb->user_handler = handler;
    gpio_init_callback(&cb->cb, cb_trampoline, pin_mask);
}

int nn_osal_gpio_add_callback(nn_osal_gpio_pin_t *p,
                              nn_osal_gpio_callback_t *cb)
{
    if (!p || !p->port || !cb) return -EINVAL;
    return gpio_add_callback(p->port, &cb->cb);
}

int nn_osal_gpio_interrupt_configure(nn_osal_gpio_pin_t *p, uint32_t flags)
{
    if (!p || !p->port) return -EINVAL;
    gpio_flags_t z = 0;
    if (flags & NN_OSAL_GPIO_INT_EDGE_RISING)  z |= GPIO_INT_EDGE_RISING;
    if (flags & NN_OSAL_GPIO_INT_EDGE_FALLING) z |= GPIO_INT_EDGE_FALLING;
    if (flags & NN_OSAL_GPIO_INT_LEVEL_HIGH)   z |= GPIO_INT_LEVEL_HIGH;
    if (flags & NN_OSAL_GPIO_INT_LEVEL_LOW)    z |= GPIO_INT_LEVEL_LOW;
    if (flags & NN_OSAL_GPIO_INT_DISABLE)      z |= GPIO_INT_DISABLE;
    return gpio_pin_interrupt_configure(p->port, p->pin, z);
}

bool nn_osal_gpio_port_ready(nn_osal_gpio_port_t *port)
{
    return port && device_is_ready((const struct device *)port);
}
