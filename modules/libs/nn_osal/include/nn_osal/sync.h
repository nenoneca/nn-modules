/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * nn_osal/sync.h — semaphores + mutexes.
 *
 * Both are blocking primitives with a millisecond timeout (use
 * NN_OSAL_WAIT_FOREVER to block until signalled, or 0 to poll
 * non-blocking).
 */

#define NN_OSAL_WAIT_FOREVER  (-1)
#define NN_OSAL_NO_WAIT       (0)

typedef struct nn_osal_sem    nn_osal_sem_t;
typedef struct nn_osal_mutex  nn_osal_mutex_t;

/* ── semaphore ─────────────────────────────────────────────────────── */

/* Initialise *sem with initial = `initial_count`, max = `max_count`. */
int nn_osal_sem_init(nn_osal_sem_t *sem, unsigned initial_count,
                     unsigned max_count);

/* Take (P).  timeout_ms in milliseconds, or NN_OSAL_WAIT_FOREVER /
 * NN_OSAL_NO_WAIT.  Returns 0 on success, -EAGAIN on timeout. */
int nn_osal_sem_take(nn_osal_sem_t *sem, int timeout_ms);

/* Give (V).  Always non-blocking. */
void nn_osal_sem_give(nn_osal_sem_t *sem);

/* Reset to 0 (drain any pending count). */
void nn_osal_sem_reset(nn_osal_sem_t *sem);

unsigned nn_osal_sem_count_get(const nn_osal_sem_t *sem);

/* ── mutex ─────────────────────────────────────────────────────────── */

int  nn_osal_mutex_init(nn_osal_mutex_t *mtx);
int  nn_osal_mutex_lock(nn_osal_mutex_t *mtx, int timeout_ms);
void nn_osal_mutex_unlock(nn_osal_mutex_t *mtx);

/* Pull in opaque struct definitions for the active backend. */
#include "internal/backend_sync.h"
