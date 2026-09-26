/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * Public crypto primitives backed by the device's provisioned X25519 identity
 * (PSA: X25519 + HKDF-SHA256 + AES-256-GCM).  Used by nn_sectun to build
 * authenticated secure sessions (stream + control channel) that reuse the same
 * key material the hub provisioned.  The device's static private key never
 * leaves nn_prov; callers reach it only via nn_prov_crypto_device_x25519().
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Generate an ephemeral X25519 keypair. */
esp_err_t nn_prov_crypto_gen_x25519(uint8_t pub[32], uint8_t priv[32]);

/* X25519(priv, peer) — caller supplies the private key (e.g. an ephemeral). */
esp_err_t nn_prov_crypto_x25519(const uint8_t priv[32], const uint8_t peer[32],
                                uint8_t out[32]);

/* X25519(device_static_priv, peer) — the device key never leaves nn_prov. */
esp_err_t nn_prov_crypto_device_x25519(const uint8_t peer[32], uint8_t out[32]);

/* HKDF-SHA256(ikm, salt, info) → out[out_len]. */
esp_err_t nn_prov_crypto_hkdf(const uint8_t *ikm, size_t ikm_len,
                              const uint8_t *salt, size_t salt_len,
                              const uint8_t *info, size_t info_len,
                              uint8_t *out, size_t out_len);

/* AES-256-GCM one-shot.  decrypt=false → encrypt (out = ct||16B tag);
 * decrypt=true → verify+decrypt.  *out_len in/out (capacity → result len). */
esp_err_t nn_prov_crypto_aesgcm(bool decrypt, const uint8_t key[32],
                                const uint8_t nonce[12],
                                const uint8_t *aad, size_t aad_len,
                                const uint8_t *in, size_t in_len,
                                uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
