/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#ifdef CONFIG_NN_OSAL_BACKEND_ZEPHYR
#  include <zephyr/kernel.h>

/* Wrappers each carry the user's handler so the work-queue trampoline
 * can recover it via CONTAINER_OF.  Previous versions reinterpret-cast
 * the user handler directly into k_work_init / k_work_init_delayable —
 * which relied on the embedded struct k_work being at offset 0 inside
 * struct k_work_delayable.  That assumption was wrong on Zephyr 4.x
 * (struct _timeout precedes the work field), so the reschedule path
 * walked into garbage memory and crashed.  Fixed by adding an explicit
 * user_handler field + a real trampoline in src/zephyr/work.c.
 */

struct nn_osal_work {
    struct k_work inner;
    void (*user_handler)(struct nn_osal_work *);
};

struct nn_osal_work_delayable {
    struct k_work_delayable inner;
    void (*user_handler)(struct nn_osal_work_delayable *);
};

struct nn_osal_work_queue { struct k_work_q inner; };

#elif defined(CONFIG_NN_OSAL_BACKEND_AMEBA)
#  include "FreeRTOS.h"
#  include "task.h"
#  include "queue.h"
#  include "timers.h"
/* FreeRTOS has no work-queue primitive, so this is a task draining a queue of
 * work pointers, with delayable items held by a software timer that posts to
 * the same queue when it fires.
 *
 * Note the Zephyr comment above: reinterpret-casting the user handler relied
 * on an embedded struct being at offset 0 and broke on Zephyr 4.x.  Keep the
 * explicit user_handler field here for the same reason -- do not "simplify"
 * it back into a cast. */
struct nn_osal_work {
    void       *next;
    void      (*user_handler)(struct nn_osal_work *);
};
struct nn_osal_work_delayable {
    struct nn_osal_work  work;
    StaticTimer_t        timer_buf;
    TimerHandle_t        timer;
    void               (*user_handler)(struct nn_osal_work_delayable *);
};
struct nn_osal_work_queue {
    StaticTask_t    tcb;
    TaskHandle_t    task;
    StaticQueue_t   q_buf;
    QueueHandle_t   q;
};
#else
#  error "no nn_osal backend selected (ZEPHYR / ESP_IDF / POSIX / AMEBA)"
#endif
