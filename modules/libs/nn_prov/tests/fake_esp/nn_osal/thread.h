/* SPDX-License-Identifier: Apache-2.0 */
/* Fake nn_osal/thread.h for the nn_prov host test.
 *
 * Deliberately does NOT run the entry function.  nn_prov's only thread is
 * the deferred-reboot task, so running it for real would reboot whatever
 * machine the test suite is on. */
#pragma once
#include <stddef.h>

typedef struct nn_osal_thread { int dummy; } nn_osal_thread_t;
typedef void (*nn_osal_thread_entry_t)(void *a, void *b, void *c);

#define NN_OSAL_THREAD_STACK_DEFINE(name, size) char name[size]

int nn_osal_thread_create(nn_osal_thread_t *thread, void *stack,
                          size_t stack_size, nn_osal_thread_entry_t entry,
                          void *a, void *b, void *c,
                          int prio, const char *name);
