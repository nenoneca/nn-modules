/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/thread.h>
#include <zephyr/kernel.h>

int nn_osal_thread_create(nn_osal_thread_t *t,
                          void *stack, size_t stack_size,
                          nn_osal_thread_entry_t entry,
                          void *a, void *b, void *c,
                          int prio, const char *name)
{
    if (!t || !stack || !entry) return -EINVAL;
    t->tid = k_thread_create(&t->inner,
                             (k_thread_stack_t *)stack, stack_size,
                             (k_thread_entry_t)entry, a, b, c,
                             prio, 0, K_NO_WAIT);
    if (!t->tid) return -ENOMEM;
    if (name) k_thread_name_set(t->tid, name);
    return 0;
}

void nn_osal_thread_yield(void) { k_yield(); }

void nn_osal_thread_suspend(nn_osal_thread_t *t)
{
    if (t && t->tid) k_thread_suspend(t->tid);
}

void nn_osal_thread_resume(nn_osal_thread_t *t)
{
    if (t && t->tid) k_thread_resume(t->tid);
}

int nn_osal_thread_name_set(nn_osal_thread_t *t, const char *name)
{
    if (!t || !t->tid) return -EINVAL;
    return k_thread_name_set(t->tid, name);
}
