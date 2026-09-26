/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * nn_osal/thread.h — preemptive thread creation.
 *
 * Threads have a name, stack, entry, priority (lower number = higher
 * priority, matching Zephyr's convention), and optional opaque user
 * pointer.  Stack size and lifetime are caller-owned to mirror Zephyr's
 * K_THREAD_STACK_DEFINE pattern.
 *
 * Usage:
 *   NN_OSAL_THREAD_STACK_DEFINE(my_stack, 2048);
 *   nn_osal_thread_t my_thread;
 *
 *   void entry(void *a, void *b, void *c) { ... }
 *
 *   nn_osal_thread_create(&my_thread, my_stack, 2048,
 *                         entry, NULL, NULL, NULL,
 *                         5, "my-thread");
 */

typedef struct nn_osal_thread nn_osal_thread_t;

typedef void (*nn_osal_thread_entry_t)(void *a, void *b, void *c);

/* Backend-specific macro: defines a stack array with proper alignment.
 * On Zephyr this expands to K_THREAD_STACK_DEFINE. */
#define NN_OSAL_THREAD_STACK_DEFINE(name, size) \
    _NN_OSAL_THREAD_STACK_DEFINE_IMPL(name, size)

/* Create + start a thread.  Returns 0 on success, negative errno on
 * failure.  *thread is caller-allocated storage; the backend will
 * initialise it in-place.
 *
 * Lower `prio` = higher priority (matches Zephyr).
 */
int nn_osal_thread_create(nn_osal_thread_t *thread,
                          void *stack, size_t stack_size,
                          nn_osal_thread_entry_t entry,
                          void *arg1, void *arg2, void *arg3,
                          int prio, const char *name);

/* Cooperative yield to the scheduler. */
void nn_osal_thread_yield(void);

/* Suspend / resume; suspend blocks until resume.  Used by, e.g., the
 * flash-write path to halt the logging thread while writing flash. */
void nn_osal_thread_suspend(nn_osal_thread_t *thread);
void nn_osal_thread_resume(nn_osal_thread_t *thread);

/* Set the human-readable thread name (for debug output / logs). */
int nn_osal_thread_name_set(nn_osal_thread_t *thread, const char *name);

/* Pull in the backend's NN_OSAL_THREAD_STACK_DEFINE definition. */
#include "internal/backend_thread.h"
