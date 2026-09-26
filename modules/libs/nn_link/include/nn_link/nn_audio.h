/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include "nn_link/nn_link.h"

/*
 * nn_audio — AAC audio transport over nn_link (P4 -> C6).
 *
 * One nn_link packet = [nn_aud_hdr_t][one AAC (ADTS) frame].  An AAC-LC frame
 * at 16 kHz mono is a few hundred bytes, so audio is NEVER fragmented — each
 * packet is a self-contained frame.  Like video, every packet carries the P4
 * device timestamp (esp_timer ms) so the C6 can reorder audio + video by time
 * before the uplink.
 */

#define NN_AUD_MAGIC        0x41u   /* 'A' — distinct from 'V' 0x56, 'O' 0x4F */

typedef struct __attribute__((packed)) {
    uint8_t  magic;     /* NN_AUD_MAGIC                                   */
    uint8_t  flags;     /* reserved (0)                                  */
    uint16_t seq;       /* audio frame sequence number                   */
    uint32_t ts_ms;     /* device capture timestamp (esp_timer ms)       */
} nn_aud_hdr_t;

#define NN_AUD_HDR_LEN      ((int)sizeof(nn_aud_hdr_t))      /* 8 */
#define NN_AUD_MAX_PAYLOAD  (NN_LINK_MAX_PACKET - NN_AUD_HDR_LEN)
