/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/osal.h>

#include "button_base.h"

NN_OSAL_LOG_MODULE(button);

NN_OSAL_BUILD_ASSERT_DT_ALIAS_OK(sw0,
    "node_mgr/button needs a 'sw0' alias in devicetree "
    "(see boards/<board>.overlay)");

/* DT-time extraction → runtime nn_osal_gpio handle.  All Zephyr DT
 * macros are hidden inside the OSAL backend; libs only see the
 * (port, pin, active-low) triple. */
#define BTN_PORT   NN_OSAL_GPIO_DT_PORT_BY_ALIAS(sw0)
#define BTN_PIN    NN_OSAL_GPIO_DT_PIN_BY_ALIAS(sw0)
#define BTN_AL     NN_OSAL_GPIO_DT_ACTIVE_LOW(sw0)

static nn_osal_gpio_pin_t       g_btn;
static nn_osal_gpio_callback_t  g_cb;
static struct k_work_delayable  g_debounce;
static button_settled_cb_t      g_on_settled;
static void                    *g_user;
static int                      g_last_state;

static void debounce_work_fn(struct k_work *work)
{
	(void)work;

	int v = nn_osal_gpio_get_logical(&g_btn);
	if (v < 0) {
		NN_LOG_WRN("nn_osal_gpio_get_logical: %d", v);
		return;
	}
	int held = !!v;  /* honours ACTIVE_LOW via the flag below */

	if (held == g_last_state) {
		return;
	}
	g_last_state = held;
	NN_LOG_DBG("button %s", held ? "pressed" : "released");

	if (g_on_settled) {
		g_on_settled(held, g_user);
	}
}

static void btn_isr(nn_osal_gpio_pin_t *pin,
		    nn_osal_gpio_callback_t *cb,
		    uint32_t pins)
{
	(void)pin; (void)cb; (void)pins;
	/* Re-arm the debounce window on every edge. */
	k_work_reschedule(&g_debounce,
			  K_MSEC(CONFIG_NODE_MGR_BUTTON_DEBOUNCE_MS));
}

int button_base_init(button_settled_cb_t on_settled, void *user)
{
	nn_osal_gpio_port_t *port = BTN_PORT;
	if (!nn_osal_gpio_port_ready(port)) {
		NN_LOG_ERR("button GPIO port not ready");
		return -ENODEV;
	}

	g_on_settled = on_settled;
	g_user       = user;
	g_last_state = 0;

	nn_osal_gpio_from_raw(&g_btn, port, BTN_PIN, 0);
	nn_osal_gpio_register("sw0", &g_btn);

	/* Translate DT active-low flag → nn_osal flag bit. */
	uint32_t flags = NN_OSAL_GPIO_INPUT;
	flags |= BTN_AL ? NN_OSAL_GPIO_ACTIVE_LOW : NN_OSAL_GPIO_ACTIVE_HIGH;

	int rc = nn_osal_gpio_configure(&g_btn, flags);
	if (rc) {
		NN_LOG_ERR("nn_osal_gpio_configure: %d", rc);
		return rc;
	}
	rc = nn_osal_gpio_interrupt_configure(&g_btn,
					      NN_OSAL_GPIO_INT_EDGE_BOTH);
	if (rc) {
		NN_LOG_ERR("nn_osal_gpio_interrupt_configure: %d", rc);
		return rc;
	}
	nn_osal_gpio_callback_init(&g_cb, btn_isr, BIT(BTN_PIN));
	rc = nn_osal_gpio_add_callback(&g_btn, &g_cb);
	if (rc) {
		NN_LOG_ERR("nn_osal_gpio_add_callback: %d", rc);
		return rc;
	}
	k_work_init_delayable(&g_debounce, debounce_work_fn);

	NN_LOG_INF("button on sw0 pin %d (active_%s), debounce=%dms",
		BTN_PIN, BTN_AL ? "low" : "high",
		CONFIG_NODE_MGR_BUTTON_DEBOUNCE_MS);
	return 0;
}

int button_get_state(void)
{
	return g_last_state;
}
