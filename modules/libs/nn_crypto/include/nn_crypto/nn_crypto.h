/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_crypto — the converged ECIES v2 + session-crypto library (slice 2 of the
 * nn_osal convergence).  ONE implementation, built on both Zephyr and ESP-IDF
 * because it speaks only PSA (psa/crypto.h, common to both) + nn_osal for key
 * persistence (nn_osal_kv) and the PSA serialization lock (nn_osal_mutex).
 *
 * Merges device/modules/libs/fw_common/hub_crypto.c (Zephyr) and
 * media/components/nn_prov/nn_prov_crypto.c (ESP-IDF).  Wire-compatible with
 * hub/hub/crypto.py: ECIES v2 = X25519 static-static + ephemeral ECDH →
 * HKDF-SHA256 (salt=epk, info="nn-hub-v2-{h2d,d2h}") → AES-256-GCM, envelope
 * {"v":2,"epk":b64,"nonce":b64,"ct":b64}.
 *
 * Return convention: 0 / negative errno.  All public calls are serialized
 * against each other (PSA is not thread-safe in the Zephyr tf-psa build).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* psa_crypto_init + load (or first-boot generate + persist via nn_osal_kv,
 * key "nn_crypto/dev_x25519_priv") the device's long-term X25519 identity.
 * Also loads a previously-stored peer (hub) pubkey if present. */
int nn_crypto_init(void);

/* Copy the device's 32-byte X25519 public key. */
void nn_crypto_device_pub(uint8_t out[32]);

/* Store + persist the peer (hub) long-term X25519 public key (needed for the
 * static-static DH leg of ECIES). */
int nn_crypto_set_hub_pub(const uint8_t hub_pub[32]);

/* True once both the device key and the hub pubkey are available. */
bool nn_crypto_ready(void);

/* ECIES v2 hub→device decrypt of a JSON envelope.  *out_len in/out. */
int nn_crypto_decrypt_h2d(const char *json, size_t json_len,
                          uint8_t *out, size_t *out_len);

/* ECIES v2 device→hub encrypt → JSON envelope into json_out (cap json_cap). */
int nn_crypto_encrypt_d2h(const uint8_t *plain, size_t plain_len,
                          char *json_out, size_t json_cap);

/* ── Low-level primitives (for nn_sectun secure sessions) ───────────────── */
int nn_crypto_gen_x25519(uint8_t pub[32], uint8_t priv[32]);
int nn_crypto_x25519(const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32]);
int nn_crypto_device_x25519(const uint8_t peer[32], uint8_t out[32]);
int nn_crypto_hkdf(const uint8_t *ikm, size_t ikm_len,
                   const uint8_t *salt, size_t salt_len,
                   const uint8_t *info, size_t info_len,
                   uint8_t *out, size_t out_len);
int nn_crypto_aesgcm(bool decrypt, const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *in, size_t in_len,
                     uint8_t *out, size_t out_cap, size_t *out_len);

/* Test-only: inject a device private key (bypasses kv) so host known-answer
 * tests can run without storage.  Returns 0. */
int nn_crypto_test_set_device_priv(const uint8_t priv[32]);

#ifdef __cplusplus
}
#endif
