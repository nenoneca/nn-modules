/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_prov_crypto — CONVERGED SHIM.
 *
 * The crypto that used to live here is now the shared nn_crypto library
 * (device/modules/libs/nn_crypto), built on both Zephyr and ESP-IDF over
 * nn_osal.  This file keeps the nn_prov_crypto_* surface so nn_prov.c /
 * nn_prov_ble.c are unchanged, delegating every call to nn_crypto.  This makes
 * provisioning + nn_sectun share ONE device X25519 identity + one audited
 * ECIES implementation.
 */
#include "nn_prov_crypto.h"
#include "nn_crypto/nn_crypto.h"

esp_err_t nn_prov_crypto_init(void) { return nn_crypto_init(); }

void nn_prov_crypto_device_pub(uint8_t out[32]) { nn_crypto_device_pub(out); }

void nn_prov_crypto_set_hub_pub(const uint8_t hub_pub[32]) { nn_crypto_set_hub_pub(hub_pub); }

bool nn_prov_crypto_ready(void) { return nn_crypto_ready(); }

esp_err_t nn_prov_crypto_decrypt_h2d(const char *json, size_t json_len,
                                     uint8_t *out, size_t *out_len)
{
    return nn_crypto_decrypt_h2d(json, json_len, out, out_len);
}

/* Primitives (public header) — delegate too, for any remaining callers. */
esp_err_t nn_prov_crypto_gen_x25519(uint8_t pub[32], uint8_t priv[32])
{
    return nn_crypto_gen_x25519(pub, priv);
}

esp_err_t nn_prov_crypto_x25519(const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32])
{
    return nn_crypto_x25519(priv, peer, out);
}

esp_err_t nn_prov_crypto_device_x25519(const uint8_t peer[32], uint8_t out[32])
{
    return nn_crypto_device_x25519(peer, out);
}

esp_err_t nn_prov_crypto_hkdf(const uint8_t *ikm, size_t ikm_len,
                              const uint8_t *salt, size_t salt_len,
                              const uint8_t *info, size_t info_len,
                              uint8_t *out, size_t out_len)
{
    return nn_crypto_hkdf(ikm, ikm_len, salt, salt_len, info, info_len, out, out_len);
}

esp_err_t nn_prov_crypto_aesgcm(bool decrypt, const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *aad, size_t aad_len,
                                const uint8_t *in, size_t in_len,
                                uint8_t *out, size_t out_cap, size_t *out_len)
{
    return nn_crypto_aesgcm(decrypt, key, nonce, aad, aad_len, in, in_len, out, out_cap, out_len);
}
