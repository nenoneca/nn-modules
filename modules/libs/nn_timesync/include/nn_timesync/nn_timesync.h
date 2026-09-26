/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_timesync (C6) — absolute (hub-synced) wall clock + P4 clock mapping.
 *
 *   hub --SET_TIME(epoch_ms)--> C6 : sets the absolute clock, NOTIFIES the P4.
 *   C6  --NOTIFY-->  P4  --REQ(p4_timer)-->  C6 : C6 learns the P4<->abs offset.
 *   relay: nn_timesync_p4_to_abs(packet.p4_ts) -> absolute epoch ms for the record.
 */

#ifdef __cplusplus
extern "C" {
#endif

void     nn_timesync_init(void);

/* Hub pushed a new absolute time (epoch ms): set the clock + notify the P4. */
void     nn_timesync_set_epoch(uint64_t epoch_ms);

/* Current absolute time in epoch ms (0 until first hub sync). */
uint64_t nn_timesync_abs_ms(void);

/* Map a P4 esp_timer-ms stamp to absolute epoch ms (uses the measured P4
 * offset; falls back to relay-time, then to the raw stamp if unsynced). */
uint64_t nn_timesync_p4_to_abs(uint32_t p4_ts_ms);

/* Handle an inbound NN_TIME_MAGIC packet from the P4 (the REQ). */
void     nn_timesync_on_p4_msg(const uint8_t *data, size_t len);

bool     nn_timesync_synced(void);
void     nn_timesync_status(char *out, size_t n);

#ifdef __cplusplus
}
#endif
