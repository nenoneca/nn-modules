/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_ota_c6 — C6-side OTA orchestrator (lifecycle master).
 *
 * Driven by the hub over the encrypted control channel (nn_ctrl): the hub
 * pushes the staged image, verifies each one, arms, then triggers the swap.
 * The C6 stages ITS OWN image via nn_ota_dfu, and relays the P4's image over
 * nn_link (nn_ota_link sub-protocol).  On apply it swaps the P4 first, confirms
 * the P4 reached the target version, then swaps itself.
 *
 * Targets:
 */
#define NN_OTA_TARGET_P4  0u
#define NN_OTA_TARGET_C6  1u

/* Control opcodes carried over nn_ctrl (req[0]); replies echo the opcode then
 * a 1-byte status (0=ok) unless noted.  Kept distinct from PING/STATUS. */
#define NN_OTA_OP_OFFER   0x10u  /* JSON {p4:{ver,size,sha},c6:{...}} -> status */
#define NN_OTA_OP_BEGIN   0x11u  /* [u8 target][u32 LE size]          -> status */
#define NN_OTA_OP_DATA    0x12u  /* [u8 target][u32 LE off][bytes]    -> status */
#define NN_OTA_OP_VERIFY  0x13u  /* [u8 target][u32 LE size][32B sha] -> status */
#define NN_OTA_OP_ARM     0x14u  /* (no body)                         -> status */
#define NN_OTA_OP_APPLY   0x15u  /* (no body)  P4-first then C6 self   -> status */
#define NN_OTA_OP_STATUS  0x16u  /* (no body)  -> JSON status string            */

/* Largest OTA data block pushed per DATA record (fits one nn_link packet to
 * the P4: NN_OTA_HDR_LEN + this <= NN_LINK_MAX_PACKET). */
#define NN_OTA_BLOCK      1008u

#ifdef __cplusplus
extern "C" {
#endif

/* Create sync primitives + reconcile armed state on boot (self-confirm if we
 * just swapped successfully). */
void nn_ota_c6_init(void);

/* Inbound P4->C6 OTA messages (VERSION/CKSUM/ACK), routed by the C6 app from
 * nn_link packets whose first byte is NN_OTA_MAGIC. */
void nn_ota_c6_on_p4_msg(const uint8_t *data, size_t len);

/* ── control-op handlers (called from nn_ctrl dispatch) ──────────────────── */
int    nn_ota_c6_offer(const uint8_t *body, size_t n);                 /* 0=accept */
int    nn_ota_c6_begin(uint8_t target, uint32_t size);
int    nn_ota_c6_data(uint8_t target, uint32_t off, const uint8_t *b, size_t n);
int    nn_ota_c6_verify(uint8_t target, uint32_t size, const uint8_t sha[32]); /* 0=match */
int    nn_ota_c6_arm(void);
int    nn_ota_c6_apply(void);                                          /* 0=ok (reboots) */
size_t nn_ota_c6_status(char *buf, size_t cap);                        /* JSON length */

#ifdef __cplusplus
}
#endif
