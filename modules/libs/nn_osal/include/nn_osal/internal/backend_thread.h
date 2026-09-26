/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/*
 * Backend-specific thread types + the NN_OSAL_THREAD_STACK_DEFINE
 * macro implementation.  Only the headers under nn_osal/ should ever
 * #include this file directly.
 */

#ifdef CONFIG_NN_OSAL_BACKEND_ZEPHYR
#  include <zephyr/kernel.h>
struct nn_osal_thread { struct k_thread inner; k_tid_t tid; };
#  define _NN_OSAL_THREAD_STACK_DEFINE_IMPL(name, size) \
        K_THREAD_STACK_DEFINE(name, size)
#elif defined(CONFIG_NN_OSAL_BACKEND_ESP_IDF)
#  include "freertos/FreeRTOS.h"
#  include "freertos/task.h"
/* FreeRTOS has no k_thread 3-arg entry, so the struct carries a trampoline:
 * a static TCB + the caller's entry and its three args (see src/esp_idf/
 * thread.c).  The stack is a caller-owned StackType_t array (the macro). */
struct nn_osal_thread {
    StaticTask_t            tcb;
    TaskHandle_t            inner;
    nn_osal_thread_entry_t  entry;
    void                   *a, *b, *c;
};
/* Zephyr stack size is bytes; FreeRTOS stacks are arrays of StackType_t. */
#  define _NN_OSAL_THREAD_STACK_DEFINE_IMPL(name, size) \
        StackType_t name[((size) + sizeof(StackType_t) - 1) / sizeof(StackType_t)]
#elif defined(CONFIG_NN_OSAL_BACKEND_POSIX)
#  include <pthread.h>
/* pthreads: detached thread + the same 3-arg trampoline as the FreeRTOS
 * backend.  The caller-provided stack is IGNORED (glibc manages stacks);
 * the macro still defines it so call sites stay backend-agnostic. */
struct nn_osal_thread {
    pthread_t               inner;
    nn_osal_thread_entry_t  entry;
    void                   *a, *b, *c;
};
#  define _NN_OSAL_THREAD_STACK_DEFINE_IMPL(name, size) \
        __attribute__((unused)) unsigned char name[1]   /* unused on POSIX */
#elif defined(CONFIG_NN_OSAL_BACKEND_AMEBA)
#  include "FreeRTOS.h"
#  include "task.h"
/* AmebaPro2 (RTL8735B) is FreeRTOS, so this is the ESP-IDF shape: a static
 * TCB plus the 3-arg trampoline FreeRTOS lacks natively.  The two backends
 * differ only in where the FreeRTOS headers live, which is why this file is
 * the only place that difference appears. */
struct nn_osal_thread {
    StaticTask_t            tcb;
    TaskHandle_t            inner;
    nn_osal_thread_entry_t  entry;
    void                   *a, *b, *c;
};
#  define _NN_OSAL_THREAD_STACK_DEFINE_IMPL(name, size) \
        StackType_t name[((size) + sizeof(StackType_t) - 1) / sizeof(StackType_t)]
#else
#  error "no nn_osal backend selected (ZEPHYR / ESP_IDF / POSIX / AMEBA)"
#endif
