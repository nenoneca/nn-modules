/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE   /* pthread_setname_np */
/* nn_osal thread backend — POSIX (pthreads).
 *
 * Mirrors the FreeRTOS backend's trampoline (3-arg entry) and priority
 * inversion handling: OSAL prio (lower = higher, Zephyr sense) is mapped to
 * SCHED_OTHER niceness-free threads — hosted Linux schedules fairly, and the
 * nn libs use priorities as hints, not hard guarantees.  The caller-owned
 * stack is ignored (glibc manages stacks). */
#include "nn_osal/thread.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

static void *trampoline(void *p)
{
    nn_osal_thread_t *t = (nn_osal_thread_t *)p;
    t->entry(t->a, t->b, t->c);
    return NULL;
}

int nn_osal_thread_create(nn_osal_thread_t *thread,
                          void *stack, size_t stack_size,
                          nn_osal_thread_entry_t entry,
                          void *arg1, void *arg2, void *arg3,
                          int prio, const char *name)
{
    (void)stack; (void)stack_size; (void)prio;
    thread->entry = entry;
    thread->a = arg1; thread->b = arg2; thread->c = arg3;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&thread->inner, &at, trampoline, thread);
    pthread_attr_destroy(&at);
    if (r == 0 && name)
        pthread_setname_np(thread->inner, name);
    return r == 0 ? 0 : -r;
}

void nn_osal_thread_yield(void) { sched_yield(); }

/* pthreads has no external suspend/resume.  The only in-tree user is the
 * C6 flash-XIP writer, which never builds for POSIX; log loudly if a new
 * caller appears rather than pretending it worked. */
void nn_osal_thread_suspend(nn_osal_thread_t *thread)
{
    (void)thread;
    fprintf(stderr, "nn_osal(posix): thread_suspend unsupported\n");
}
void nn_osal_thread_resume(nn_osal_thread_t *thread)
{
    (void)thread;
    fprintf(stderr, "nn_osal(posix): thread_resume unsupported\n");
}

int nn_osal_thread_name_set(nn_osal_thread_t *thread, const char *name)
{
    return pthread_setname_np(thread->inner, name) == 0 ? 0 : -1;
}
