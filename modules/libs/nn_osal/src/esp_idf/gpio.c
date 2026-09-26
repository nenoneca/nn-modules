/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal GPIO surface — ESP-IDF backend.  Mirrors src/zephyr/gpio.c.
 * ESP-IDF GPIO is a flat pin space, so `port` is ignored and `pin` is the
 * gpio_num_t.  Logical level honours NN_OSAL_GPIO_ACTIVE_LOW (cached in
 * pin->flags), matching the Zephyr backend's gpio_pin_get/set semantics. */
#if defined(CONFIG_NN_OSAL_BACKEND_ESP_IDF)

#include <nn_osal/gpio.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include <string.h>
#include <errno.h>

/* ── named-pin registry (identical contract to the Zephyr backend) ─── */

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
    for (unsigned i = 0; i < s_reg_count; i++) {
        if (s_reg[i].name && strcmp(s_reg[i].name, name) == 0) {
            s_reg[i].pin = pin;          /* allow re-registration */
            return 0;
        }
    }
    if (s_reg_count >= NN_OSAL_GPIO_REG_MAX) return -ENOMEM;
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
    if (!pin) return -EINVAL;
    pin->port  = (void *)port;           /* unused on ESP-IDF */
    pin->pin   = pin_number;
    pin->flags = flags;
    return 0;
}

/* ── runtime API ────────────────────────────────────────────────── */

int nn_osal_gpio_configure(nn_osal_gpio_pin_t *p, uint32_t flags)
{
    if (!p) return -EINVAL;
    p->flags = flags;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << p->pin,
        .mode         = GPIO_MODE_DISABLE,
        .pull_up_en   = (flags & NN_OSAL_GPIO_PULL_UP)   ? 1 : 0,
        .pull_down_en = (flags & NN_OSAL_GPIO_PULL_DOWN) ? 1 : 0,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    const bool in  = flags & NN_OSAL_GPIO_INPUT;
    const bool out = flags & NN_OSAL_GPIO_OUTPUT;
    const bool od  = flags & NN_OSAL_GPIO_OPEN_DRAIN;
    if (in && out)      cfg.mode = od ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT;
    else if (out)       cfg.mode = od ? GPIO_MODE_OUTPUT_OD       : GPIO_MODE_OUTPUT;
    else if (in)        cfg.mode = GPIO_MODE_INPUT;

    esp_err_t e = gpio_config(&cfg);
    if (e != ESP_OK) return -EIO;

    /* Honour the requested init level for outputs (raw level after ACTIVE_LOW). */
    if (out && (flags & (NN_OSAL_GPIO_INIT_HIGH | NN_OSAL_GPIO_INIT_LOW))) {
        int logical = (flags & NN_OSAL_GPIO_INIT_HIGH) ? 1 : 0;
        nn_osal_gpio_set_logical(p, logical);
    }
    return 0;
}

int nn_osal_gpio_get_logical(const nn_osal_gpio_pin_t *p)
{
    if (!p) return -EINVAL;
    int raw = gpio_get_level((gpio_num_t)p->pin);
    return (p->flags & NN_OSAL_GPIO_ACTIVE_LOW) ? !raw : raw;
}

int nn_osal_gpio_set_logical(nn_osal_gpio_pin_t *p, int value)
{
    if (!p) return -EINVAL;
    int raw = (p->flags & NN_OSAL_GPIO_ACTIVE_LOW) ? !value : !!value;
    return gpio_set_level((gpio_num_t)p->pin, raw) == ESP_OK ? 0 : -EIO;
}

int nn_osal_gpio_toggle(nn_osal_gpio_pin_t *p)
{
    if (!p) return -EINVAL;
    return nn_osal_gpio_set_logical(p, !nn_osal_gpio_get_logical(p));
}

bool nn_osal_gpio_port_ready(nn_osal_gpio_port_t *port)
{
    (void)port;     /* ESP-IDF GPIO is always available once the driver links */
    return true;
}

/* ── interrupts (per-pin ISR via the shared gpio ISR service) ──────── */

static void IRAM_ATTR isr_trampoline(void *arg)
{
    nn_osal_gpio_callback_t *cb = (nn_osal_gpio_callback_t *)arg;
    if (cb && cb->user_handler) cb->user_handler(NULL, cb, cb->pin_mask);
}

void nn_osal_gpio_callback_init(nn_osal_gpio_callback_t *cb,
                                nn_osal_gpio_cb_t handler,
                                uint32_t pin_mask)
{
    if (!cb) return;
    cb->user_handler = handler;
    cb->pin_mask     = pin_mask;
}

int nn_osal_gpio_add_callback(nn_osal_gpio_pin_t *p,
                              nn_osal_gpio_callback_t *cb)
{
    if (!p || !cb) return -EINVAL;
    static bool s_isr_service;
    if (!s_isr_service) {
        esp_err_t e = gpio_install_isr_service(0);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return -EIO;
        s_isr_service = true;
    }
    return gpio_isr_handler_add((gpio_num_t)p->pin, isr_trampoline, cb) == ESP_OK
               ? 0 : -EIO;
}

int nn_osal_gpio_interrupt_configure(nn_osal_gpio_pin_t *p, uint32_t flags)
{
    if (!p) return -EINVAL;
    gpio_int_type_t t = GPIO_INTR_DISABLE;
    if (flags & NN_OSAL_GPIO_INT_EDGE_BOTH)        t = GPIO_INTR_ANYEDGE;
    else if (flags & NN_OSAL_GPIO_INT_EDGE_RISING) t = GPIO_INTR_POSEDGE;
    else if (flags & NN_OSAL_GPIO_INT_EDGE_FALLING)t = GPIO_INTR_NEGEDGE;
    else if (flags & NN_OSAL_GPIO_INT_LEVEL_HIGH)  t = GPIO_INTR_HIGH_LEVEL;
    else if (flags & NN_OSAL_GPIO_INT_LEVEL_LOW)   t = GPIO_INTR_LOW_LEVEL;
    return gpio_set_intr_type((gpio_num_t)p->pin, t) == ESP_OK ? 0 : -EIO;
}

#endif /* CONFIG_NN_OSAL_BACKEND_ESP_IDF */
