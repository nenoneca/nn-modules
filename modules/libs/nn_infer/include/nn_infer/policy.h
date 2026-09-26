/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Per-class capture policy with hysteresis, evaluated identically on the
 * device (this file) and in the media service (infer_policy.py).
 *
 * Aggregation: a ring of the last `agg` per-frame confidences for the class
 * (the frame's HIGHEST confidence for it, or 0 when absent), aggregated as
 * the MEAN — one lucky frame must not start a recording.  The update is
 * O(1) via a running sum, and the ring holds conf_x1000 INTEGERS so the sum
 * is exact: a float running sum would drift over millions of updates and
 * desync the two implementations.
 *
 *   idle   --(agg >= start)--> detect     (capture video)
 *   detect --(agg <  stop )--> idle       (event may close)
 * Invariant: stop <= start (clamped by the hub on write; re-clamped here so
 * a hand-written config can't create a state machine that never leaves
 * detect). */

#define NN_INFER_AGG_MAX 30

typedef struct {
    bool     capture;        /* class may start/extend an event */
    uint8_t  agg;            /* window length, 1..NN_INFER_AGG_MAX (default 5) */
    uint16_t start_x1000;    /* enter detect at aggregate >= this */
    uint16_t stop_x1000;     /* leave detect at aggregate <  this (<= start) */
} nn_infer_class_policy_t;

typedef struct {
    nn_infer_class_policy_t cfg;
    uint16_t ring[NN_INFER_AGG_MAX];
    uint8_t  head;
    uint32_t sum;            /* == sum(ring[0..agg-1]), maintained O(1) */
    bool     detected;       /* current state */
} nn_infer_class_state_t;

/* (Re)configure a class.  Clamps agg and stop, and RESETS the window — a
 * changed window length has no meaningful history. */
void nn_infer_policy_set(nn_infer_class_state_t *st,
                         const nn_infer_class_policy_t *cfg);

/* Feed this frame's highest confidence for the class (0 if absent).
 * Returns the current aggregate in conf_x1000 units; *changed (optional) is
 * set true when the detect state flipped on this update. */
uint16_t nn_infer_policy_feed(nn_infer_class_state_t *st, uint16_t conf_x1000,
                              bool *changed);

static inline bool nn_infer_policy_detected(const nn_infer_class_state_t *st)
{ return st->detected; }
