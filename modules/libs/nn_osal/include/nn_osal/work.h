/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_osal/work.h — deferred work + work queues.
 *
 * A "work item" is a function the OSAL runs on a worker thread.  An
 * "immediate" work item is fired as soon as a queue slot is free; a
 * "delayable" work item is run after a millisecond delay.
 *
 * Why use this instead of just `nn_osal_thread_create`: cheaper than
 * a thread per task, and the system queue is shared.  Long-running
 * work should use a dedicated queue to avoid blocking other handlers
 * (matches Zephyr's k_work_queue pattern).
 */

typedef struct nn_osal_work       nn_osal_work_t;
typedef struct nn_osal_work_delayable nn_osal_work_delayable_t;
typedef struct nn_osal_work_queue nn_osal_work_queue_t;

typedef void (*nn_osal_work_handler_t)(nn_osal_work_t *work);
typedef void (*nn_osal_work_delayable_handler_t)(nn_osal_work_delayable_t *dw);

/* ── basic work ────────────────────────────────────────────────────── */

void nn_osal_work_init(nn_osal_work_t *work,
                       nn_osal_work_handler_t handler);

/* Submit to the system work queue.  Returns 1 if newly queued, 0 if
 * already pending, negative errno on failure. */
int nn_osal_work_submit(nn_osal_work_t *work);

/* Submit to a specific work queue. */
int nn_osal_work_submit_to_queue(nn_osal_work_queue_t *q,
                                 nn_osal_work_t *work);

/* Returns non-zero if the item is queued or currently running. */
int nn_osal_work_busy(const nn_osal_work_t *work);

/* ── delayable work ────────────────────────────────────────────────── */

void nn_osal_work_delayable_init(nn_osal_work_delayable_t *dw,
                                 nn_osal_work_delayable_handler_t handler);

int  nn_osal_work_schedule(nn_osal_work_delayable_t *dw, uint32_t delay_ms);
int  nn_osal_work_reschedule(nn_osal_work_delayable_t *dw, uint32_t delay_ms);
int  nn_osal_work_cancel_delayable(nn_osal_work_delayable_t *dw);

/* Extract the delayable from inside its handler — equivalent to
 * Zephyr's k_work_delayable_from_work().  Used in shared handlers. */
nn_osal_work_delayable_t *nn_osal_work_delayable_from_work(nn_osal_work_t *w);

/* ── work queues ───────────────────────────────────────────────────── */

/* Start a dedicated work queue with its own thread.  Caller owns the
 * stack memory; declare with NN_OSAL_THREAD_STACK_DEFINE. */
int nn_osal_work_queue_start(nn_osal_work_queue_t *q,
                             void *stack, size_t stack_size,
                             int prio, const char *name);

/* Pull in opaque struct definitions for the active backend. */
#include "internal/backend_work.h"
