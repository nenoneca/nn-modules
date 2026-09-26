/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#ifdef CONFIG_NN_OSAL_BACKEND_ZEPHYR
#  include <zephyr/init.h>
   /* Map nn_osal init priority to Zephyr's APPLICATION level. */
#  define _NN_OSAL_INIT_IMPL(fn, level)                                   \
        SYS_INIT(fn, APPLICATION, level)
   /* Earlier hook — runs at POST_KERNEL, before drivers/L2 bring-up. */
#  define _NN_OSAL_INIT_EARLY_IMPL(fn, level)                             \
        SYS_INIT(fn, POST_KERNEL, level)
#elif defined(CONFIG_NN_OSAL_BACKEND_ESP_IDF)
   /* ESP-IDF has no SYS_INIT level system; map to C constructors, which run
    * during startup BEFORE app_main and the scheduler.  Use only for hooks
    * that register state (not ones needing FreeRTOS) — prefer an explicit
    * init() from app_main otherwise (see the header note).  level (0..99)
    * orders within the constructor priority band. */
#  define _NN_OSAL_INIT_IMPL(fn, level)                                   \
        __attribute__((constructor(101 + ((level) % 100))))               \
        static void _nn_osal_init_##fn(void) { (void)fn(); }
#  define _NN_OSAL_INIT_EARLY_IMPL(fn, level)                             \
        __attribute__((constructor(101 + ((level) % 100))))               \
        static void _nn_osal_einit_##fn(void) { (void)fn(); }
#elif defined(CONFIG_NN_OSAL_BACKEND_POSIX)
/* Hosted Linux: C constructors, same rationale as the ESP-IDF mapping. */
#  define _NN_OSAL_INIT_IMPL(fn, level)                                   \
        __attribute__((constructor(101 + ((level) % 100))))               \
        static void _nn_osal_init_##fn(void) { (void)fn(); }
#elif defined(CONFIG_NN_OSAL_BACKEND_AMEBA)
/* Newlib runs __libc_init_array before main, so C constructors work here the
 * same way they do on POSIX and ESP-IDF. */
#  define _NN_OSAL_INIT_IMPL(fn, level)                                   \
        __attribute__((constructor(101 + ((level) % 100))))               \
        static void _nn_osal_init_##fn(void) { (void)fn(); }
#else
#  error "no nn_osal backend selected (ZEPHYR / ESP_IDF / POSIX / AMEBA)"
#endif
