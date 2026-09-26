/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#ifdef CONFIG_NN_OSAL_BACKEND_ZEPHYR
#  include <zephyr/drivers/gpio.h>
#  include <zephyr/device.h>
#  include <zephyr/devicetree.h>

/* The opaque "port" on Zephyr is just `const struct device *` —
 * boards typically obtain via DEVICE_DT_GET(DT_NODELABEL(gpio0)) and
 * pass the result into nn_osal_gpio_register / nn_osal_gpio_from_raw.
 * No type alias here — nn_osal_gpio_port_t stays opaque from app
 * code's perspective, with the cast hidden inside the backend impl. */

struct nn_osal_gpio_pin {
    const struct device *port;   /* Zephyr GPIO controller device */
    uint32_t pin;                /* pin number within the port */
    uint32_t flags;              /* cached config flags */
};

struct nn_osal_gpio_callback {
    struct gpio_callback cb;
    nn_osal_gpio_cb_t   user_handler;
};

/* Convenience: pass any const struct device * pointer wherever
 * nn_osal_gpio_port_t* is expected.  The backend handles the cast. */
#  define NN_OSAL_GPIO_PORT(dev)  ((nn_osal_gpio_port_t *)(dev))

/* Devicetree extraction — maps to Zephyr's DT macros for the
 * `node = &gpioX 12 GPIO_ACTIVE_LOW;` shape used by `gpios` / `sw0`
 * properties.  All of these are compile-time expressions. */
#  define _NN_OSAL_GPIO_DT_PORT_BY_ALIAS_IMPL(alias) \
        NN_OSAL_GPIO_PORT(DEVICE_DT_GET(DT_GPIO_CTLR(DT_ALIAS(alias), gpios)))
#  define _NN_OSAL_GPIO_DT_PIN_BY_ALIAS_IMPL(alias) \
        DT_GPIO_PIN(DT_ALIAS(alias), gpios)
#  define _NN_OSAL_GPIO_DT_ACTIVE_LOW_IMPL(alias) \
        ((DT_GPIO_FLAGS(DT_ALIAS(alias), gpios) & GPIO_ACTIVE_LOW) != 0)
#  define _NN_OSAL_BUILD_ASSERT_DT_ALIAS_OK_IMPL(alias, msg) \
        BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_ALIAS(alias), okay), msg)
#  define _NN_OSAL_GPIO_DT_PORT_BY_NODELABEL_IMPL(label) \
        NN_OSAL_GPIO_PORT(DEVICE_DT_GET(DT_NODELABEL(label)))
#elif defined(CONFIG_NN_OSAL_BACKEND_ESP_IDF)
#  include <stdint.h>

/* ESP-IDF GPIO is a flat pin space (no per-port controller device), so the
 * opaque "port" is unused — `pin` is the raw gpio_num_t.  The struct shape
 * mirrors the Zephyr one so app code (nn_link handshake line, nn_p4ctl
 * boot/reset) stays backend-agnostic. */
struct nn_osal_gpio_pin {
    void    *port;    /* unused on ESP-IDF (kept for shape parity) */
    uint32_t pin;     /* gpio_num_t */
    uint32_t flags;   /* cached config flags (incl. ACTIVE_LOW for logical) */
};

struct nn_osal_gpio_callback {
    nn_osal_gpio_cb_t user_handler;
    uint32_t          pin_mask;
};

/* Pass a gpio_num_t directly where nn_osal_gpio_port_t* is expected; the
 * ESP-IDF backend ignores the port and keys off the pin number. */
#  define NN_OSAL_GPIO_PORT(dev)  ((nn_osal_gpio_port_t *)(dev))
#elif defined(CONFIG_NN_OSAL_BACKEND_POSIX)
#  include <stdint.h>
/* Linux: no flat pin space and no devicetree — pins are logical entries in
 * the named registry, backed by in-memory state (real hardware access, e.g.
 * libgpiod on the BeagleY-AI, plugs in behind the same struct later). */
struct nn_osal_gpio_pin {
    void    *port;    /* unused (shape parity) */
    uint32_t pin;
    uint32_t flags;
    int      level;   /* in-memory logical level */
};
struct nn_osal_gpio_callback {
    nn_osal_gpio_cb_t user_handler;
    uint32_t          pin_mask;
};
#  define NN_OSAL_GPIO_PORT(dev)  ((nn_osal_gpio_port_t *)(dev))
#elif defined(CONFIG_NN_OSAL_BACKEND_AMEBA)
#  include <stdint.h>
/* Shape parity with the other MCU backends; the real implementation binds to
 * the SDK's own gpio_t / gpio_irq_t handles in component/mbed/hal. */
struct nn_osal_gpio_pin {
    void    *port;    /* unused (shape parity) */
    uint32_t pin;     /* SDK PinName */
    uint32_t flags;   /* cached config flags (incl. ACTIVE_LOW for logical) */
};
struct nn_osal_gpio_callback {
    nn_osal_gpio_cb_t user_handler;
    uint32_t          pin_mask;
};
#  define NN_OSAL_GPIO_PORT(dev)  ((nn_osal_gpio_port_t *)(dev))
#else
#  error "no nn_osal backend selected (ZEPHYR / ESP_IDF / POSIX / AMEBA)"
#endif
