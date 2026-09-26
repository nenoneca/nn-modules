/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Gateway identity — ECDSA P-256 (secp256r1) keypair persisted in NVS.
 *
 * Signature scheme: PSA_ALG_DETERMINISTIC_ECDSA(SHA-256) on the sign
 * side (RFC 6979 deterministic nonce — no per-signature RNG needed,
 * removing the classic ECDSA-on-embedded nonce-reuse footgun).  The
 * resulting sigs are interoperable with regular ECDSA verifiers.
 *
 * On first boot the gateway generates a fresh P-256 keypair, stores
 * the private scalar under settings key `gw_identity/p256_priv`, and
 * exposes:
 *
 *   - gw_identity_get_id()      — 8-byte gateway_id (= SHA256(pubkey)[:8])
 *   - gw_identity_get_pubkey()  — 65-byte uncompressed P-256 public key
 *                                  (0x04 || X[32] || Y[32])
 *   - gw_identity_sign()        — Deterministic-ECDSA signature over a
 *                                  message, raw R||S (64 bytes)
 *   - gw_identity_verify()      — verify with a caller-supplied 65-byte
 *                                  uncompressed P-256 public key
 *
 * Sign + verify are signature-compatible with nn_proto_sign_fn and
 * nn_proto_verify_fn so they can be passed straight into the protocol
 * lib.
 */

#ifndef GW_IDENTITY_H_
#define GW_IDENTITY_H_

#include <stddef.h>
#include <stdint.h>

#define GW_IDENTITY_ID_LEN     8
#define GW_IDENTITY_PUBKEY_LEN 65   /* uncompressed: 0x04 || X || Y */
#define GW_IDENTITY_PRIV_LEN   32   /* P-256 scalar */
#define GW_IDENTITY_SIG_LEN    64   /* raw R||S */

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the keystore.  Loads from NVS or generates+persists. */
int gw_identity_init(void);

/* Pointers into static memory; valid for the lifetime of the program. */
const uint8_t *gw_identity_get_id(void);     /* 8 bytes */
const uint8_t *gw_identity_get_pubkey(void); /* 65 bytes */

/* Sign a message with the gateway's P-256 private key, deterministic
 * ECDSA over SHA-256.  sig_out must point to a 64-byte buffer (raw R || S).
 * Matches nn_proto_sign_fn signature: ctx is unused.
 */
int gw_identity_sign(void *ctx,
		     const uint8_t *msg, size_t msg_len,
		     uint8_t sig_out[64]);

/* Verify a signature with a caller-supplied 65-byte uncompressed P-256
 * public key.  ctx_pubkey points to the 65-byte public key.
 * Matches nn_proto_verify_fn signature.
 */
int gw_identity_verify(void *ctx_pubkey,
		       const uint8_t *msg, size_t msg_len,
		       const uint8_t sig[64]);

#ifdef __cplusplus
}
#endif

#endif /* GW_IDENTITY_H_ */
