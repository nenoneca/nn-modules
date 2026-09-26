/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/watchdog.h>
#include <errno.h>

#if defined(CONFIG_TASK_WDT)

#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>

static nn_osal_wdt_panic_cb_t g_panic_cb;
static void                  *g_panic_user;

static void wdt_callback(int channel, void *user_data)
{
    (void)user_data;
    if (g_panic_cb) {
        g_panic_cb(channel, g_panic_user);
    }
    /* task_wdt's contract: returning from the callback continues,
     * and a HW reset will follow when the underlying HW WDT expires. */
}

int nn_osal_task_wdt_init(uint32_t hw_period_ms,
                          nn_osal_wdt_panic_cb_t panic_cb,
                          void *user)
{
    (void)hw_period_ms;   /* HW period is fixed in DT for this backend */
    g_panic_cb   = panic_cb;
    g_panic_user = user;
#if DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
    const struct device *hw = DEVICE_DT_GET(DT_ALIAS(watchdog0));
    if (!device_is_ready(hw)) return -ENODEV;
    return task_wdt_init(hw);
#else
    return task_wdt_init(NULL);
#endif
}

nn_osal_wdt_channel_t nn_osal_task_wdt_add(uint32_t timeout_ms)
{
    return task_wdt_add(timeout_ms, wdt_callback, NULL);
}

int nn_osal_task_wdt_feed(nn_osal_wdt_channel_t channel)
{
    return task_wdt_feed(channel);
}

#else   /* CONFIG_TASK_WDT not enabled */

int nn_osal_task_wdt_init(uint32_t hw_period_ms,
                          nn_osal_wdt_panic_cb_t panic_cb,
                          void *user)
{
    (void)hw_period_ms; (void)panic_cb; (void)user;
    return -ENOSYS;
}

nn_osal_wdt_channel_t nn_osal_task_wdt_add(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return -ENOSYS;
}

int nn_osal_task_wdt_feed(nn_osal_wdt_channel_t channel)
{
    (void)channel;
    return -ENOSYS;
}

#endif
