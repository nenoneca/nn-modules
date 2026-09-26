/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal sync backend — POSIX (mutex+cond counting semaphore).
 * sem_timedwait() is CLOCK_REALTIME-based and jumps with wall-clock changes;
 * this implementation waits on CLOCK_MONOTONIC like the RTOS backends. */
#include "nn_osal/sync.h"
#include <pthread.h>
#include <time.h>
#include <errno.h>

static void abs_deadline(struct timespec *ts, int timeout_ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec  += timeout_ms / 1000;
    ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

int nn_osal_sem_init(nn_osal_sem_t *sem, unsigned initial_count, unsigned max_count)
{
    if (max_count == 0) max_count = 1;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_mutex_init(&sem->m, NULL);
    int r = pthread_cond_init(&sem->c, &ca);
    pthread_condattr_destroy(&ca);
    sem->count = initial_count;
    sem->max   = max_count;
    return r == 0 ? 0 : -ENOMEM;
}

int nn_osal_sem_take(nn_osal_sem_t *sem, int timeout_ms)
{
    int rc = 0;
    pthread_mutex_lock(&sem->m);
    if (timeout_ms == NN_OSAL_WAIT_FOREVER) {
        while (sem->count == 0)
            pthread_cond_wait(&sem->c, &sem->m);
    } else if (timeout_ms <= 0) {
        if (sem->count == 0) rc = -EAGAIN;
    } else {
        struct timespec dl;
        abs_deadline(&dl, timeout_ms);
        while (sem->count == 0 && rc == 0)
            if (pthread_cond_timedwait(&sem->c, &sem->m, &dl) == ETIMEDOUT)
                rc = -EAGAIN;
    }
    if (rc == 0 && sem->count > 0) sem->count--;
    else if (rc == 0) rc = -EAGAIN;
    pthread_mutex_unlock(&sem->m);
    return rc;
}

void nn_osal_sem_give(nn_osal_sem_t *sem)
{
    pthread_mutex_lock(&sem->m);
    if (sem->count < sem->max) sem->count++;
    pthread_cond_signal(&sem->c);
    pthread_mutex_unlock(&sem->m);
}

void nn_osal_sem_reset(nn_osal_sem_t *sem)
{
    pthread_mutex_lock(&sem->m);
    sem->count = 0;
    pthread_mutex_unlock(&sem->m);
}

unsigned nn_osal_sem_count_get(const nn_osal_sem_t *sem)
{
    return sem->count;    /* racy by nature, same as the RTOS backends */
}

int nn_osal_mutex_init(nn_osal_mutex_t *mtx)
{
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);  /* FreeRTOS mutexes are */
    int r = pthread_mutex_init(&mtx->m, &ma);
    pthread_mutexattr_destroy(&ma);
    return r == 0 ? 0 : -ENOMEM;
}

int nn_osal_mutex_lock(nn_osal_mutex_t *mtx, int timeout_ms)
{
    if (timeout_ms == NN_OSAL_WAIT_FOREVER)
        return pthread_mutex_lock(&mtx->m) == 0 ? 0 : -EAGAIN;
    if (timeout_ms <= 0)
        return pthread_mutex_trylock(&mtx->m) == 0 ? 0 : -EAGAIN;
    struct timespec dl;                        /* CLOCK_REALTIME per POSIX */
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec  += timeout_ms / 1000;
    dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    return pthread_mutex_timedlock(&mtx->m, &dl) == 0 ? 0 : -EAGAIN;
}

void nn_osal_mutex_unlock(nn_osal_mutex_t *mtx) { pthread_mutex_unlock(&mtx->m); }
