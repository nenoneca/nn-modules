/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal thread backend — ESP-IDF (FreeRTOS static tasks).
 *
 * Two impedance mismatches vs Zephyr, handled here:
 *   - entry signature: Zephyr passes 3 args; FreeRTOS passes 1 → trampoline.
 *   - priority sense: Zephyr lower = higher; FreeRTOS higher = higher → invert. */
#include "nn_osal/thread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void trampoline(void *p)
{
    nn_osal_thread_t *t = (nn_osal_thread_t *)p;
    t->entry(t->a, t->b, t->c);
    vTaskDelete(NULL);   /* entry returned — end the task cleanly */
}

int nn_osal_thread_create(nn_osal_thread_t *thread,
                          void *stack, size_t stack_size,
                          nn_osal_thread_entry_t entry,
                          void *arg1, void *arg2, void *arg3,
                          int prio, const char *name)
{
    thread->entry = entry;
    thread->a = arg1; thread->b = arg2; thread->c = arg3;

    /* Zephyr prio (lower = higher) → FreeRTOS (higher = higher), clamped to
     * (0, configMAX_PRIORITIES). */
    int fp = (configMAX_PRIORITIES - 1) - prio;
    if (fp < 1) fp = 1;
    if (fp > configMAX_PRIORITIES - 1) fp = configMAX_PRIORITIES - 1;

    thread->inner = xTaskCreateStatic(trampoline, name ? name : "nn",
                                      stack_size / sizeof(StackType_t), thread,
                                      (UBaseType_t)fp, (StackType_t *)stack,
                                      &thread->tcb);
    return thread->inner ? 0 : -1;
}

void nn_osal_thread_yield(void) { taskYIELD(); }

void nn_osal_thread_suspend(nn_osal_thread_t *thread) { vTaskSuspend(thread->inner); }
void nn_osal_thread_resume(nn_osal_thread_t *thread)  { vTaskResume(thread->inner); }

int nn_osal_thread_name_set(nn_osal_thread_t *thread, const char *name)
{
    /* FreeRTOS task names are fixed at creation; nothing to do. */
    (void)thread; (void)name;
    return 0;
}
