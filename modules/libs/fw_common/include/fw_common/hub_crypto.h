/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * hub_crypto — device-side ECIES v2 crypto for hub ↔ device messaging.
 *
 * Protocol: Noise-IK-style static+ephemeral X25519 ECDH, no Ed25519.
 *   shared_e = ECDH(ephem_priv, peer_static_pub)
 *   shared_s = ECDH(own_static_priv, peer_static_pub)
 *   aes_key  = HKDF-SHA256(shared_e || shared_s, salt=epk, info=dir_tag)
 *   ciphertext = AES-256-GCM(aes_key, nonce, plaintext)
 *
 * Envelope JSON (null-terminated string):
 *   {"v":2,"epk":"<base64>","nonce":"<base64>","ct":"<base64>"}
 *
 * NVS keys (Zephyr settings):
 *   hub_crypto/dev_x25519_priv  (32 B) — device long-term X25519 private key
 *   hub_crypto/hub_x25519_pub   (32 B) — hub long-term X25519 public key
 *
 * Call hub_crypto_init() once at boot (after settings_load()).
 * Call hub_crypto_set_hub_pubkey() when hub config is received via BLE.
 */

/** Initialise PSA and load/generate device key pair. Call after settings_load(). */
int hub_crypto_init(void);

/** Copy device X25519 public key (32 bytes) into *out. */
void hub_crypto_get_device_x25519_pub(uint8_t out[32]);

/**
 * Static-static X25519 shared secret X25519(device_priv, hub_pub).
 * Both device and hub can compute the identical 32-byte value with no
 * messages exchanged (the hub does X25519(hub_priv, device_pub)).  This
 * is the root secret for symmetric session keys (see nn_session).
 * Returns 0 on success, -EINVAL if either key is unset.
 */
int hub_crypto_static_ecdh(uint8_t out[32]);

/**
 * Store hub X25519 public key (received via BLE HUB_CONFIG_CHAR) in NVS.
 * hub_x25519_pub is 32 raw bytes.
 */
int hub_crypto_set_hub_pubkey(const uint8_t hub_x25519_pub[32]);

/**
 * Decrypt a JSON ECIES v2 envelope from the hub.
 *
 * @param json_in    NUL-terminated JSON envelope string (may be modified)
 * @param plain_out  output buffer for plaintext
 * @param plain_len  in: capacity; out: bytes written
 *
 * Returns 0 on success, negative on error (bad format, no hub key, auth fail).
 */
int hub_crypto_decrypt(char *json_in, uint8_t *plain_out, size_t *plain_len);

/**
 * Encrypt plaintext into a JSON ECIES v2 envelope destined for the hub.
 *
 * @param plain      plaintext bytes
 * @param plain_len  length of plaintext
 * @param json_out   output buffer for NUL-terminated JSON
 * @param json_cap   capacity of json_out (≥ 256 bytes recommended)
 *
 * Returns 0 on success, negative on error.
 */
int hub_crypto_encrypt(const uint8_t *plain, size_t plain_len,
                       char *json_out, size_t json_cap);
