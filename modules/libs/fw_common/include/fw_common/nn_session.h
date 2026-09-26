/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_session — symmetric authenticated-encryption session for nn_proto,
 * replacing per-frame ECDSA-sign + ECIES on the hot path.
 *
 * Root secret: the static-static X25519 shared secret both ends already
 * hold after BLE provisioning (hub_crypto_static_ecdh) — no handshake DH.
 *
 * Per-boot freshness: each side contributes an 8-byte random salt (the
 * "session nonce"); the session key is
 *   HKDF-SHA256(ecdh, salt = dev_salt || hub_salt,
 *               info = "nn-sess-v1/" + dir)   -> 32-byte AES key per dir.
 * A fresh salt pair every boot means the GCM message counter can safely
 * restart at 0 — the lesson from nn_sectun's counter-reuse InvalidTag
 * loop: never resync counters after a break, re-key instead.
 *
 * Record nonce (12 B, NOT on the wire beyond the counter): 4 zero bytes
 * || u64 BE per-direction counter.  The 8-byte counter IS emitted at the
 * front of each sealed record so the peer can reconstruct the nonce and
 * reject replays/reorders.
 *
 * AAD: the caller passes the frame header bytes it wants authenticated
 * (nn_proto magic/type/id, inner cmd/tid) so a tampered header fails the
 * tag without being encrypted.
 *
 * Everything is AES-256-GCM via PSA (software today, ~tens of µs/frame
 * on the C6 — see the selftest); no asymmetric op per message.
 */

#define NN_SESSION_SALT_LEN   8
#define NN_SESSION_KEY_LEN    32
#define NN_SESSION_TAG_LEN    16
#define NN_SESSION_CTR_LEN    8
/* bytes added to a plaintext when sealed: 8B counter + 16B GCM tag */
#define NN_SESSION_OVERHEAD   (NN_SESSION_CTR_LEN + NN_SESSION_TAG_LEN)

typedef struct {
	/* PSA key handles imported ONCE at derive() and reused for every
	 * seal/open — importing per message cost ~600 us on the C6, more
	 * than the AES-GCM itself.  Opaque uint32 to avoid a psa header in
	 * this public interface. */
	uint32_t tx_key;
	uint32_t rx_key;
	uint64_t ctr_tx;         /* next tx counter */
	uint64_t ctr_rx_hi;      /* highest rx counter accepted so far */
	uint64_t rx_window;      /* bitmap: bit i = (ctr_rx_hi - i) seen */
	bool     rx_any;         /* any record accepted yet */
	bool     established;
} nn_session_t;

/* Destroy the session's PSA key handles.  Call before re-deriving into
 * the same struct or when tearing a session down. */
void nn_session_free(nn_session_t *s);

/*
 * Derive session keys from the static ECDH secret + the two salts.
 * *is_device* picks which HKDF direction label maps to k_tx vs k_rx so
 * device and hub end up with mirrored key pairs (device k_tx == hub k_rx).
 * *ecdh* is the 32-byte output of hub_crypto_static_ecdh().
 * Returns 0 on success.
 */
int nn_session_derive(nn_session_t *s,
		      const uint8_t ecdh[NN_SESSION_KEY_LEN],
		      const uint8_t dev_salt[NN_SESSION_SALT_LEN],
		      const uint8_t hub_salt[NN_SESSION_SALT_LEN],
		      bool is_device);

/*
 * Seal: out = [ctr:8 BE][ciphertext || tag].  AAD is authenticated but
 * not encrypted.  *out_len set to plaintext len + NN_SESSION_OVERHEAD.
 * Returns 0 on success, -errno on failure / short buffer.
 */
int nn_session_seal(nn_session_t *s,
		    const uint8_t *aad, size_t aad_len,
		    const uint8_t *pt, size_t pt_len,
		    uint8_t *out, size_t out_cap, size_t *out_len);

/*
 * Open: parse [ctr:8][ct||tag], verify tag over aad+ct, enforce
 * ctr > highest-seen (monotonic anti-replay), decrypt into *out.
 * Returns 0 on success, -EBADMSG on auth fail, -EEXIST on replay.
 */
int nn_session_open(nn_session_t *s,
		    const uint8_t *aad, size_t aad_len,
		    const uint8_t *in, size_t in_len,
		    uint8_t *out, size_t out_cap, size_t *out_len);
