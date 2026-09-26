/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * nn_osal/gpio.h — digital GPIO, edge interrupts, named-pin lookup.
 *
 * Two ways to obtain a pin handle:
 *
 *   1. Named lookup (P6b — recommended):
 *        nn_osal_gpio_pin_t *led = nn_osal_gpio_get("led0");
 *      The name maps to a board-defined pin via NN_OSAL_GPIO_REGISTER()
 *      called from the board's `nn_board.c`.  Apps reference pins by
 *      logical name only; the (port, pin, flags) translation lives in
 *      the board layer.
 *
 *   2. Raw port+pin (escape hatch for boards still on DT macros):
 *        nn_osal_gpio_pin_t *p = nn_osal_gpio_from_raw(port, pin);
 *      `port` is an opaque, backend-specific handle (Zephyr: const
 *      struct device *; POSIX: TBD).  Use this only inside board code.
 *
 * The runtime API (configure / get / set / toggle / callback) takes the
 * opaque handle and is identical for both lookup paths.
 */

typedef struct nn_osal_gpio_pin nn_osal_gpio_pin_t;
typedef struct nn_osal_gpio_callback nn_osal_gpio_callback_t;

/* Flags for nn_osal_gpio_configure. */
#define NN_OSAL_GPIO_INPUT          (1 << 0)
#define NN_OSAL_GPIO_OUTPUT         (1 << 1)
#define NN_OSAL_GPIO_PULL_UP        (1 << 2)
#define NN_OSAL_GPIO_PULL_DOWN      (1 << 3)
#define NN_OSAL_GPIO_ACTIVE_HIGH    (1 << 4)
#define NN_OSAL_GPIO_ACTIVE_LOW     (1 << 5)
#define NN_OSAL_GPIO_INIT_HIGH      (1 << 6)
#define NN_OSAL_GPIO_INIT_LOW       (1 << 7)
#define NN_OSAL_GPIO_OPEN_DRAIN     (1 << 8)

#define NN_OSAL_GPIO_INT_EDGE_RISING   (1 << 0)
#define NN_OSAL_GPIO_INT_EDGE_FALLING  (1 << 1)
#define NN_OSAL_GPIO_INT_EDGE_BOTH     (NN_OSAL_GPIO_INT_EDGE_RISING | \
                                        NN_OSAL_GPIO_INT_EDGE_FALLING)
#define NN_OSAL_GPIO_INT_LEVEL_HIGH    (1 << 2)
#define NN_OSAL_GPIO_INT_LEVEL_LOW     (1 << 3)
#define NN_OSAL_GPIO_INT_DISABLE       (1 << 4)

typedef void (*nn_osal_gpio_cb_t)(nn_osal_gpio_pin_t *pin,
                                  nn_osal_gpio_callback_t *cb,
                                  uint32_t pins);

/* ── named-pin registry ─────────────────────────────────────────── */

/* Backend-opaque handle wrapping the platform port (e.g. on Zephyr,
 * const struct device *).  Boards call NN_OSAL_GPIO_REGISTER once per
 * named pin from board init; apps then call nn_osal_gpio_get(name). */
typedef struct nn_osal_gpio_port nn_osal_gpio_port_t;

/* Look up a board-defined pin by logical name.  Returns NULL if no
 * registration with that name exists.  Safe to call after the board's
 * init hook has run (typically very early in boot, before app
 * threads start). */
nn_osal_gpio_pin_t *nn_osal_gpio_get(const char *logical_name);

/* Register a pin handle under a logical name.  Boards call this from
 * their nn_board.c at SYS_INIT priority.  *pin storage is caller-owned
 * (typically a file-static); the registry stores only a pointer. */
int nn_osal_gpio_register(const char *logical_name,
                          nn_osal_gpio_pin_t *pin);

/* Construct a pin handle from a (backend-opaque port, pin number, flags)
 * triple.  Storage is caller-owned; this just initialises in place. */
int nn_osal_gpio_from_raw(nn_osal_gpio_pin_t *pin,
                          nn_osal_gpio_port_t *port,
                          uint32_t pin_number,
                          uint32_t flags);

/* ── runtime API (works for both named + raw handles) ─────────── */

int nn_osal_gpio_configure(nn_osal_gpio_pin_t *pin, uint32_t flags);
int nn_osal_gpio_get_logical(const nn_osal_gpio_pin_t *pin);
int nn_osal_gpio_set_logical(nn_osal_gpio_pin_t *pin, int value);
int nn_osal_gpio_toggle(nn_osal_gpio_pin_t *pin);

/* Returns true if the underlying GPIO controller is ready (driver
 * probed, IRQ table populated, etc.).  Backends that don't need this
 * concept simply return true. */
bool nn_osal_gpio_port_ready(nn_osal_gpio_port_t *port);

/* ── devicetree extraction helpers ────────────────────────────────
 *
 * Boards / libs that need to pull pin info from devicetree at compile
 * time should go through these macros instead of including
 * <zephyr/devicetree.h> + <zephyr/device.h> directly.  Each maps to the
 * matching Zephyr DT macro on the Zephyr backend; on other backends it
 * can resolve to a board-table lookup, a Kconfig string, or a no-op
 * BUILD_ASSERT.
 *
 *   Usage example (replaces the BTN_NODE / BTN_PORT / BTN_PIN trio):
 *     #define BTN_PORT  NN_OSAL_GPIO_DT_PORT_BY_ALIAS(sw0)
 *     #define BTN_PIN   NN_OSAL_GPIO_DT_PIN_BY_ALIAS(sw0)
 *     #define BTN_AL    NN_OSAL_GPIO_DT_ACTIVE_LOW(sw0)
 *     NN_OSAL_BUILD_ASSERT_DT_ALIAS_OK(sw0, "boards needs sw0 alias");
 */
#define NN_OSAL_GPIO_DT_PORT_BY_ALIAS(alias)  _NN_OSAL_GPIO_DT_PORT_BY_ALIAS_IMPL(alias)
#define NN_OSAL_GPIO_DT_PIN_BY_ALIAS(alias)   _NN_OSAL_GPIO_DT_PIN_BY_ALIAS_IMPL(alias)
#define NN_OSAL_GPIO_DT_ACTIVE_LOW(alias)     _NN_OSAL_GPIO_DT_ACTIVE_LOW_IMPL(alias)
#define NN_OSAL_BUILD_ASSERT_DT_ALIAS_OK(alias, msg) \
        _NN_OSAL_BUILD_ASSERT_DT_ALIAS_OK_IMPL(alias, msg)

/* Get the GPIO controller (port) by devicetree node label — useful when
 * a board wants to reference a top-level controller (e.g. gpio0) directly
 * rather than going through a per-pin alias.  The returned handle is
 * suitable as the `port` argument to nn_osal_gpio_from_raw(). */
#define NN_OSAL_GPIO_DT_PORT_BY_NODELABEL(label) \
        _NN_OSAL_GPIO_DT_PORT_BY_NODELABEL_IMPL(label)

/* ── interrupts ────────────────────────────────────────────────── */

void nn_osal_gpio_callback_init(nn_osal_gpio_callback_t *cb,
                                nn_osal_gpio_cb_t handler,
                                uint32_t pin_mask);
int  nn_osal_gpio_add_callback(nn_osal_gpio_pin_t *pin,
                               nn_osal_gpio_callback_t *cb);
int  nn_osal_gpio_interrupt_configure(nn_osal_gpio_pin_t *pin,
                                      uint32_t flags);

#include "internal/backend_gpio.h"
