/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#ifdef CONFIG_NN_OSAL_BACKEND_ZEPHYR
#  include <zephyr/kernel.h>
struct nn_osal_sem   { struct k_sem   inner; };
struct nn_osal_mutex { struct k_mutex inner; };
#elif defined(CONFIG_NN_OSAL_BACKEND_ESP_IDF)
#  include "freertos/FreeRTOS.h"
#  include "freertos/semphr.h"
/* Static FreeRTOS allocation: storage lives in-place (matches Zephyr's
 * caller-owned k_sem/k_mutex), the handle points into it. */
struct nn_osal_sem   { StaticSemaphore_t buf; SemaphoreHandle_t inner; };
struct nn_osal_mutex { StaticSemaphore_t buf; SemaphoreHandle_t inner; };
#elif defined(CONFIG_NN_OSAL_BACKEND_POSIX)
#  include <pthread.h>
/* Counting semaphore built on mutex+cond (sem_timedwait uses CLOCK_REALTIME;
 * this honours timeout_ms against CLOCK_MONOTONIC like the other backends). */
struct nn_osal_sem   { pthread_mutex_t m; pthread_cond_t c; unsigned count, max; };
struct nn_osal_mutex { pthread_mutex_t m; };
#elif defined(CONFIG_NN_OSAL_BACKEND_AMEBA)
#  include "FreeRTOS.h"
#  include "semphr.h"
/* Static allocation, as on ESP-IDF: storage is caller-owned and the handle
 * points into it.
 *
 * DECIDE AND DOCUMENT recursive vs non-recursive before writing sync.c, and
 * make T1's sync case assert whichever you choose.  An unstated lock
 * assumption is what deadlocked the BlueZ backend: a non-recursive mutex held
 * across a dispatch, re-entered by the callback it dispatched.  FreeRTOS has
 * both xSemaphoreCreateMutexStatic and ...RecursiveMutexStatic, so this is a
 * real choice here, not an inherited one. */
struct nn_osal_sem   { StaticSemaphore_t buf; SemaphoreHandle_t inner; };
struct nn_osal_mutex { StaticSemaphore_t buf; SemaphoreHandle_t inner; };
#else
#  error "no nn_osal backend selected (ZEPHYR / ESP_IDF / POSIX / AMEBA)"
#endif
