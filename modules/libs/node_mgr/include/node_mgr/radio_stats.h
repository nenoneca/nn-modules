/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radio_stats -- periodic RADIO_STATS report to the hub.
 *
 * Every CONFIG_NODE_MGR_RADIO_STATS_PERIOD_MS the device sends one
 * unsigned, fire-and-forget D2H RADIO_STATS (0x0045) carrying CUMULATIVE
 * counters since boot, so the hub derives rates from consecutive reports
 * and a lost report only costs resolution -- the reports travel over the
 * very channel whose health they describe.  Wire format: see
 * radio_stats.c (v1, 59 bytes, little-endian).
 */
#ifndef NODE_MGR_RADIO_STATS_H_
#define NODE_MGR_RADIO_STATS_H_

#include <stddef.h>
#include <stdint.h>

#define RADIO_STATS_V1_LEN 59

int  radio_stats_start(void);
void radio_stats_stop(void);

/* Build the v1 record into buf (>= RADIO_STATS_V1_LEN).  Returns length. */
size_t radio_stats_build(uint8_t *buf, size_t len);

#endif
