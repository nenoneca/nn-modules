/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_proto_identity — P-256 (secp256r1) keystore primitives shared
 * between the gateway broker and end devices.
 *
 * Sign uses RFC 6979 deterministic ECDSA (PSA_ALG_DETERMINISTIC_ECDSA)
 * so the embedded path doesn't need an RNG per signature.  Sigs are
 * byte-compatible with regular ECDSA verifiers.
 *
 * The library is storage-agnostic: callers own the privkey + pubkey
 * buffers and decide where to persist them (typically Zephyr settings
 * with a per-app subtree).  The library provides:
 *
 *   - keygen + export priv/pub
 *   - derive 8B node_id (= SHA256(pubkey)[:8])
 *   - sign / verify (compatible with nn_proto_sign_fn / verify_fn)
 *
 * All functions assume PSA crypto is initialised (call psa_crypto_init()
 * once before).
 */

#ifndef NN_PROTO_IDENTITY_H_
#define NN_PROTO_IDENTITY_H_

#include <stddef.h>
#include <stdint.h>

#define NN_PROTO_IDENTITY_NODE_ID_LEN  8
#define NN_PROTO_IDENTITY_PUBKEY_LEN   65   /* 0x04 || X[32] || Y[32] */
#define NN_PROTO_IDENTITY_PRIV_LEN     32   /* P-256 scalar */
#define NN_PROTO_IDENTITY_SIG_LEN      64

#ifdef __cplusplus
extern "C" {
#endif

/* Generate a fresh P-256 keypair and export both halves. */
int nn_proto_identity_keygen(uint8_t priv[NN_PROTO_IDENTITY_PRIV_LEN],
			     uint8_t pub[NN_PROTO_IDENTITY_PUBKEY_LEN]);

/* Re-derive the public key from a stored private scalar. */
int nn_proto_identity_pub_from_priv(const uint8_t priv[NN_PROTO_IDENTITY_PRIV_LEN],
				    uint8_t pub[NN_PROTO_IDENTITY_PUBKEY_LEN]);

/* node_id = SHA256(pubkey)[:8]. */
int nn_proto_identity_derive_node_id(const uint8_t pub[NN_PROTO_IDENTITY_PUBKEY_LEN],
				     uint8_t out[NN_PROTO_IDENTITY_NODE_ID_LEN]);

/* Sign with a stored private scalar (deterministic ECDSA over SHA-256).
 * Compatible with nn_proto_sign_fn — pass the priv buffer as the
 * sign_ctx. */
int nn_proto_identity_sign(void *ctx_priv32,
			   const uint8_t *msg, size_t msg_len,
			   uint8_t sig_out[64]);

/* Verify with a 65-byte uncompressed public key.  Compatible with
 * nn_proto_verify_fn — pass the pubkey buffer as the verify_ctx. */
int nn_proto_identity_verify(void *ctx_pubkey,
			     const uint8_t *msg, size_t msg_len,
			     const uint8_t sig[64]);

/* PSA serialization helpers.
 *
 * TF-PSA-crypto is not compiled thread-safe in this build (no
 * CONFIG_MBEDTLS_THREADING_C).  Concurrent psa_* calls from multiple
 * threads have been observed to wedge Mbed TLS's internal allocator,
 * after which psa_sign_message permanently returns
 * PSA_ERROR_INSUFFICIENT_MEMORY (-141) until reboot.
 *
 * `nn_proto_identity_sign` / `_verify` take this mutex internally.
 * Any OTHER caller that uses psa_*  (e.g. hub_crypto's ECIES decrypt)
 * MUST take it too, otherwise it can race with sign/verify and bring
 * everything down.  Lock is non-recursive — don't double-acquire from
 * one thread. */
void nn_proto_identity_psa_lock(void);
void nn_proto_identity_psa_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* NN_PROTO_IDENTITY_H_ */
