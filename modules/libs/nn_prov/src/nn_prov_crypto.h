/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "nn_prov/nn_prov_crypto.h"   /* public primitives (gen/x25519/hkdf/aesgcm) */

/*
 * Internal ECIES v2 crypto for nn_prov — X25519 static-static + ephemeral
 * ECDH → HKDF-SHA256 → AES-256-GCM, over the ESP-IDF PSA crypto API.  Wire
 * compatible with hub/hub/crypto.py and device fw_common/hub_crypto.c.
 */

/* psa_crypto_init() + load (or first-boot generate + persist) the device's
 * long-term X25519 key pair in NVS namespace "nnprov", key "dev_priv". */
esp_err_t nn_prov_crypto_init(void);

/* Copy the device's 32-byte X25519 public key. */
void nn_prov_crypto_device_pub(uint8_t out[32]);

/* Set the hub's long-term X25519 public key (needed for the static-static DH
 * leg of decrypt).  Held in RAM; persistence is the caller's job. */
void nn_prov_crypto_set_hub_pub(const uint8_t hub_pub[32]);

/* True once both the device key and the hub public key are available. */
bool nn_prov_crypto_ready(void);

/* Decrypt a hub→device ("h2d") ECIES v2 JSON envelope.  *out_len is in/out:
 * pass the buffer capacity, receive the plaintext length.  Returns ESP_OK or
 * an error (ESP_ERR_INVALID_STATE if keys aren't ready, ESP_FAIL on auth fail). */
esp_err_t nn_prov_crypto_decrypt_h2d(const char *json, size_t json_len,
                                     uint8_t *out, size_t *out_len);

/* Low-level primitives (gen/x25519/device_x25519/hkdf/aesgcm) are declared in
 * the public header <nn_prov/nn_prov_crypto.h>, included above. */
