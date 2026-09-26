/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_group — Phase-4 group-keyed AEAD for D2D AUTO_NOTIFY cascades.
 *
 * The hub mints a random 32-byte group key + 32-bit epoch and delivers
 * it to each device SEALED over its verified per-boot session
 * (SESS_GROUP_KEY).  Cascade notifies then travel as AUTO_NOTIFY_S:
 *
 *   record = [epoch:4 LE][ctr:8 BE][AES-256-GCM ct || 16B tag]
 *   nonce  = sender_device_id[0:4] || ctr(8 BE)
 *   ctr    = (boot_rand:32 << 32) | seq   — the random hi-word keeps
 *            nonces unique across reboots under an unrotated key
 *   AAD    = cmd(2 LE) || sender_device_id(8)
 *
 * This both REMOVES the sender's ~1 s ECDSA per target and ADDS
 * receiver-side authentication (legacy D2D was signed but receivers
 * never verified).  Two epochs are kept so a mid-rotation fleet still
 * cascades.  Replay: per-sender last-counter, with a new boot (hi-word
 * change) accepted as a fresh stream.
 */

#define NN_GROUP_KEY_LEN  32
#define NN_GROUP_HDR_LEN  12          /* epoch 4 + ctr 8 */
#define NN_GROUP_TAG_LEN  16
#define NN_GROUP_OVERHEAD (NN_GROUP_HDR_LEN + NN_GROUP_TAG_LEN)

/* Install a group key (keeps the previous epoch alive for mixed-fleet
 * rotation windows).  Called from the SESS_GROUP_KEY handler. */
int nn_group_set(uint32_t epoch, const uint8_t key[NN_GROUP_KEY_LEN]);

bool nn_group_ready(void);

/* Seal *pt* for the group as *self_id8* (our 8-byte device id). */
int nn_group_seal(const uint8_t self_id8[8], uint16_t cmd,
		  const uint8_t *pt, size_t pt_len,
		  uint8_t *out, size_t out_cap, size_t *out_len);

/* Open a record from *sender_id8*.  -EEXIST on replay, -EBADMSG on bad
 * tag, -ENOKEY-like -EINVAL when the epoch is unknown. */
int nn_group_open(const uint8_t sender_id8[8], uint16_t cmd,
		  const uint8_t *in, size_t in_len,
		  uint8_t *out, size_t out_cap, size_t *out_len);
