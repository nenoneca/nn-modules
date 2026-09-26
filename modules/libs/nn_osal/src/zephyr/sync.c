/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/sync.h>
#include <zephyr/kernel.h>
#include <errno.h>

static inline k_timeout_t to_timeout(int ms)
{
    if (ms == NN_OSAL_WAIT_FOREVER) return K_FOREVER;
    if (ms == NN_OSAL_NO_WAIT)      return K_NO_WAIT;
    return K_MSEC(ms);
}

/* ── semaphores ────────────────────────────────────────────────── */

int nn_osal_sem_init(nn_osal_sem_t *s, unsigned initial, unsigned max)
{
    if (!s) return -EINVAL;
    return k_sem_init(&s->inner, initial, max);
}

int nn_osal_sem_take(nn_osal_sem_t *s, int timeout_ms)
{
    if (!s) return -EINVAL;
    int rv = k_sem_take(&s->inner, to_timeout(timeout_ms));
    /* k_sem_take returns -EAGAIN on timeout already. */
    return rv;
}

void nn_osal_sem_give(nn_osal_sem_t *s)
{
    if (s) k_sem_give(&s->inner);
}

void nn_osal_sem_reset(nn_osal_sem_t *s)
{
    if (s) k_sem_reset(&s->inner);
}

unsigned nn_osal_sem_count_get(const nn_osal_sem_t *s)
{
    if (!s) return 0;
    /* k_sem_count_get takes a non-const pointer; we cast — read-only. */
    return k_sem_count_get((struct k_sem *)&s->inner);
}

/* ── mutexes ──────────────────────────────────────────────────── */

int nn_osal_mutex_init(nn_osal_mutex_t *m)
{
    if (!m) return -EINVAL;
    return k_mutex_init(&m->inner);
}

int nn_osal_mutex_lock(nn_osal_mutex_t *m, int timeout_ms)
{
    if (!m) return -EINVAL;
    return k_mutex_lock(&m->inner, to_timeout(timeout_ms));
}

void nn_osal_mutex_unlock(nn_osal_mutex_t *m)
{
    if (m) k_mutex_unlock(&m->inner);
}
