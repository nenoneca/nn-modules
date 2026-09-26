/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/work.h>
#include <zephyr/kernel.h>
#include <errno.h>

/* Zephyr's work-queue calls the registered handler with a `struct k_work *`
 * — for delayable work that pointer is the INTERIOR `work` field inside
 * `struct k_work_delayable`, NOT the base of the delayable struct (the
 * timeout state precedes the work field).
 *
 * Reinterpret-casting a `void (*)(nn_osal_work_delayable_t *)` into
 * `k_work_handler_t` therefore handed the user a pointer with the wrong
 * effective base — `&dw->inner` inside the handler then computed an
 * address pointing into the middle of the timeout state, and any call to
 * nn_osal_work_schedule()/cancel()/etc. corrupted memory and crashed the
 * chip ~30 s after boot.  See feedback_v2110_brick_c6_s2.md.
 *
 * Fix: a real trampoline.  The user's handler is stashed in the wrapper
 * struct (backend_work.h added the field); the trampoline recovers the
 * wrapper via CONTAINER_OF and invokes the user's handler with a properly
 * based pointer. */

static void work_trampoline(struct k_work *kw)
{
    nn_osal_work_t *w = CONTAINER_OF(kw, nn_osal_work_t, inner);
    if (w->user_handler) w->user_handler(w);
}

static void work_delayable_trampoline(struct k_work *kw)
{
    struct k_work_delayable *kdw = k_work_delayable_from_work(kw);
    nn_osal_work_delayable_t *dw =
        CONTAINER_OF(kdw, nn_osal_work_delayable_t, inner);
    if (dw->user_handler) dw->user_handler(dw);
}

void nn_osal_work_init(nn_osal_work_t *w, nn_osal_work_handler_t handler)
{
    if (!w || !handler) return;
    w->user_handler = handler;
    k_work_init(&w->inner, work_trampoline);
}

int nn_osal_work_submit(nn_osal_work_t *w)
{
    if (!w) return -EINVAL;
    return k_work_submit(&w->inner);
}

int nn_osal_work_submit_to_queue(nn_osal_work_queue_t *q, nn_osal_work_t *w)
{
    if (!q || !w) return -EINVAL;
    return k_work_submit_to_queue(&q->inner, &w->inner);
}

int nn_osal_work_busy(const nn_osal_work_t *w)
{
    if (!w) return 0;
    return k_work_busy_get(&w->inner);
}

/* ── delayable ─────────────────────────────────────────────────── */

void nn_osal_work_delayable_init(nn_osal_work_delayable_t *dw,
                                 nn_osal_work_delayable_handler_t handler)
{
    if (!dw || !handler) return;
    dw->user_handler = handler;
    k_work_init_delayable(&dw->inner, work_delayable_trampoline);
}

int nn_osal_work_schedule(nn_osal_work_delayable_t *dw, uint32_t delay_ms)
{
    if (!dw) return -EINVAL;
    return k_work_schedule(&dw->inner, K_MSEC(delay_ms));
}

int nn_osal_work_reschedule(nn_osal_work_delayable_t *dw, uint32_t delay_ms)
{
    if (!dw) return -EINVAL;
    return k_work_reschedule(&dw->inner, K_MSEC(delay_ms));
}

int nn_osal_work_cancel_delayable(nn_osal_work_delayable_t *dw)
{
    if (!dw) return -EINVAL;
    return k_work_cancel_delayable(&dw->inner);
}

nn_osal_work_delayable_t *nn_osal_work_delayable_from_work(nn_osal_work_t *w)
{
    if (!w) return NULL;
    struct k_work_delayable *kdw = k_work_delayable_from_work(&w->inner);
    return CONTAINER_OF(kdw, nn_osal_work_delayable_t, inner);
}

/* ── queues ────────────────────────────────────────────────────── */

int nn_osal_work_queue_start(nn_osal_work_queue_t *q,
                             void *stack, size_t stack_size,
                             int prio, const char *name)
{
    if (!q || !stack) return -EINVAL;
    k_work_queue_init(&q->inner);
    struct k_work_queue_config cfg = { .name = name, .no_yield = false };
    k_work_queue_start(&q->inner,
                       (k_thread_stack_t *)stack, stack_size,
                       prio, &cfg);
    return 0;
}
