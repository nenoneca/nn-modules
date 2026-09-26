/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include "nn_link/nn_link.h"

/*
 * nn_ota_link — C6<->P4 OTA control sub-protocol carried over nn_link.
 *
 * nn_link packets are demuxed by their first byte (a "magic"): video frames
 * use NN_VID_MAGIC ('V'); OTA control messages use NN_OTA_MAGIC ('O').  The
 * C6 (control master) drives the P4 (slave) through staging + swap:
 *
 *   C6 -> P4 : VERSION_REQ, BEGIN(size), CHUNK(off)+bytes, CKSUM_REQ(size), APPLY
 *   P4 -> C6 : VERSION(str), CKSUM(32B sha256), ACK(status)
 *
 * The P4 streams chunks into its INACTIVE OTA slot (esp_ota), hashes it on
 * request, and self-applies (set_boot_partition + restart) on APPLY.  The C6
 * confirms the P4 came back on the target version (via VERSION) before it
 * swaps itself — so the master can always recover a bad P4 image.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define NN_OTA_MAGIC   0x4Fu   /* 'O' — distinct from NN_VID_MAGIC 0x56 'V' */

/* Message type (hdr.type). */
enum {
    NN_OTA_MSG_VERSION     = 0x01, /* P4->C6: payload = running version string  */
    NN_OTA_MSG_VERSION_REQ = 0x02, /* C6->P4: please report your version        */
    NN_OTA_MSG_BEGIN       = 0x03, /* C6->P4: start staging; arg = total size   */
    NN_OTA_MSG_CHUNK       = 0x04, /* C6->P4: arg = offset; payload = bytes      */
    NN_OTA_MSG_CKSUM_REQ   = 0x05, /* C6->P4: arg = image size to hash          */
    NN_OTA_MSG_CKSUM       = 0x06, /* P4->C6: payload = 32-byte SHA-256          */
    NN_OTA_MSG_APPLY       = 0x07, /* C6->P4: set boot to staged slot + reboot  */
    NN_OTA_MSG_ACK         = 0x08, /* P4->C6: arg = status (0=ok, else errno)   */
    NN_OTA_MSG_CONFIRM     = 0x09, /* C6->P4: mark running image valid (no revert)*/
};

typedef struct __attribute__((packed)) {
    uint8_t  magic;     /* NN_OTA_MAGIC                      */
    uint8_t  type;      /* NN_OTA_MSG_*                      */
    uint16_t rsv;       /* reserved / alignment             */
    uint32_t arg;       /* offset (CHUNK) / size (BEGIN,     */
                        /*  CKSUM_REQ) / status (ACK) / 0    */
} nn_ota_hdr_t;

#define NN_OTA_HDR_LEN      ((int)sizeof(nn_ota_hdr_t))             /* 8 */
#define NN_OTA_MAX_PAYLOAD  (NN_LINK_MAX_PACKET - NN_OTA_HDR_LEN)   /* 1016 */
#define NN_OTA_SHA_LEN      32

#ifdef __cplusplus
}
#endif
