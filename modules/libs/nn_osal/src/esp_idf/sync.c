/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal sync backend — ESP-IDF (FreeRTOS static semaphores/mutexes). */
#include "nn_osal/sync.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <errno.h>

static TickType_t to_ticks(int ms)
{
    if (ms == NN_OSAL_WAIT_FOREVER) return portMAX_DELAY;
    if (ms <= 0)                    return 0;
    return pdMS_TO_TICKS(ms);
}

int nn_osal_sem_init(nn_osal_sem_t *sem, unsigned initial_count, unsigned max_count)
{
    if (max_count == 0) max_count = 1;
    sem->inner = xSemaphoreCreateCountingStatic(max_count, initial_count, &sem->buf);
    return sem->inner ? 0 : -ENOMEM;
}

int nn_osal_sem_take(nn_osal_sem_t *sem, int timeout_ms)
{
    return xSemaphoreTake(sem->inner, to_ticks(timeout_ms)) == pdTRUE ? 0 : -EAGAIN;
}

void nn_osal_sem_give(nn_osal_sem_t *sem)   { xSemaphoreGive(sem->inner); }

void nn_osal_sem_reset(nn_osal_sem_t *sem)
{
    while (xSemaphoreTake(sem->inner, 0) == pdTRUE) { }
}

unsigned nn_osal_sem_count_get(const nn_osal_sem_t *sem)
{
    return (unsigned)uxSemaphoreGetCount(sem->inner);
}

int nn_osal_mutex_init(nn_osal_mutex_t *mtx)
{
    mtx->inner = xSemaphoreCreateMutexStatic(&mtx->buf);
    return mtx->inner ? 0 : -ENOMEM;
}

int nn_osal_mutex_lock(nn_osal_mutex_t *mtx, int timeout_ms)
{
    return xSemaphoreTake(mtx->inner, to_ticks(timeout_ms)) == pdTRUE ? 0 : -EAGAIN;
}

void nn_osal_mutex_unlock(nn_osal_mutex_t *mtx) { xSemaphoreGive(mtx->inner); }
