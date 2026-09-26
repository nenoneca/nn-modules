/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * fw_common/envelope.h — minimal builder + parser for the ECIES v2
 * JSON envelope.  Hand-rolled (no JSON dependency) so the code is
 * the same across Zephyr and plain Linux.
 *
 *   {"v":2,"epk":"<b64>","nonce":"<b64>","ct":"<b64>"}
 *
 * Strictly enforces:
 *   - all four keys are present
 *   - epk is exactly 32 raw bytes after base64 decode
 *   - nonce is exactly 12 raw bytes
 *   - v == 2
 *
 * The `dir` parameter on the parser is opaque to the envelope itself —
 * it's used by the caller's ECIES code to pick the right HKDF info
 * tag.  Kept out of this header for that reason.
 */

struct fw_envelope_v2 {
	uint8_t  epk[32];
	uint8_t  nonce[12];
	const uint8_t *ct_b64;   /* pointer into the caller's input buffer */
	size_t   ct_b64_len;
};

/*
 * Parse a NUL-terminated JSON string `in` into the struct.
 * Returns 0 on success, -EINVAL on malformed/unsupported.
 */
int fw_envelope_v2_parse(const char *in, struct fw_envelope_v2 *out);

/*
 * Build a JSON envelope from raw ephemeral pubkey, nonce, and
 * ciphertext.  Writes into `out` (NUL-terminated).  Returns 0 on
 * success, -ENOMEM if `out_cap` too small.
 */
int fw_envelope_v2_build(char *out, size_t out_cap,
			 const uint8_t epk[32],
			 const uint8_t nonce[12],
			 const uint8_t *ct, size_t ct_len);
