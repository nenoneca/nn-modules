/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include "nn_link/nn_link.h"

/*
 * nn_time — C6<->P4 clock-sync sub-protocol over nn_link.
 *
 * The C6 owns the absolute (hub-synced) wall clock.  When the hub pushes a new
 * time to the C6, the C6 NOTIFIES the P4; the P4 answers with a REQ carrying its
 * current esp_timer value, and the C6 computes the P4<->C6 offset so it can
 * rewrite every relayed media packet's (P4-local) timestamp into absolute
 * hub-epoch milliseconds.  (The P4 keeps stamping its own esp_timer; all
 * absolute-time mapping lives on the network-connected C6.)
 *
 * Demux: nn_link first byte — 'V' video, 'A' audio, 'O' OTA, 'T' time.
 */

#define NN_TIME_MAGIC   0x54u   /* 'T' */

enum {
    NN_TIME_NOTIFY = 0x01,  /* C6 -> P4: absolute time updated, please REQ      */
    NN_TIME_REQ    = 0x02,  /* P4 -> C6: arg = P4 esp_timer ms (for offset calc)*/
};

typedef struct __attribute__((packed)) {
    uint8_t  magic;         /* NN_TIME_MAGIC            */
    uint8_t  type;          /* NN_TIME_NOTIFY / REQ     */
    uint16_t rsv;
    uint32_t p4_timer_ms;   /* REQ: P4 esp_timer ms now */
} nn_time_hdr_t;

#define NN_TIME_HDR_LEN  ((int)sizeof(nn_time_hdr_t))   /* 8 */
