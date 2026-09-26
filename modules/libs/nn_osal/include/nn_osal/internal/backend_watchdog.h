/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* No public types — task_wdt is referred to via opaque channel ids. */

#ifdef CONFIG_NN_OSAL_BACKEND_ZEPHYR
/* Implementation in src/zephyr/watchdog.c gates on CONFIG_TASK_WDT;
 * if unavailable, all calls return -ENOSYS. */
#elif defined(CONFIG_NN_OSAL_BACKEND_AMEBA)
/* No public types.  RTL8735B has a hardware watchdog; wire it rather than
 * stubbing it -- cam4 needed two operator trips for hangs a watchdog would
 * have recovered, and T1's watchdog case starves a task on purpose. */
#else
#  error "no nn_osal backend selected (ZEPHYR / ESP_IDF / POSIX / AMEBA)"
#endif
