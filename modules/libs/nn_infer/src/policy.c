/* SPDX-License-Identifier: Apache-2.0 */
#include <nn_infer/policy.h>
#include <string.h>

void nn_infer_policy_set(nn_infer_class_state_t *st,
                         const nn_infer_class_policy_t *cfg)
{
    if (!st || !cfg) return;
    memset(st, 0, sizeof *st);
    st->cfg = *cfg;
    if (st->cfg.agg == 0) st->cfg.agg = 5;                    /* default */
    if (st->cfg.agg > NN_INFER_AGG_MAX) st->cfg.agg = NN_INFER_AGG_MAX;
    if (st->cfg.stop_x1000 > st->cfg.start_x1000)             /* invariant */
        st->cfg.stop_x1000 = st->cfg.start_x1000;
    /* ring/sum/head/detected already zeroed: a class must EARN its trigger */
}

uint16_t nn_infer_policy_feed(nn_infer_class_state_t *st, uint16_t conf_x1000,
                              bool *changed)
{
    if (changed) *changed = false;
    if (!st || st->cfg.agg == 0) return 0;

    st->sum -= st->ring[st->head];        /* value leaving the window */
    st->sum += conf_x1000;
    st->ring[st->head] = conf_x1000;      /* push back */
    st->head = (uint8_t)((st->head + 1) % st->cfg.agg);

    uint16_t agg = (uint16_t)(st->sum / st->cfg.agg);
    bool was = st->detected;
    if (!st->detected) {
        if (agg >= st->cfg.start_x1000) st->detected = true;
    } else {
        if (agg < st->cfg.stop_x1000) st->detected = false;
    }
    if (changed && was != st->detected) *changed = true;
    return agg;
}
