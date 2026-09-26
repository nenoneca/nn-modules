/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * nn_prov — BLE provisioning for the ESP32-C6 media networking co-processor.
 *
 * The camera system has no keyboard and the C6 owns the only radios, so the
 * hub provisions it over BLE (Just-Works pairing, trust-on-first-provision):
 *
 *   hub (BLE central) ──GATT──▶ C6 (peripheral)
 *       reads  DEVICE_PUBKEY  (C6's X25519 public key)
 *       writes CONFIG         (name, hub+stream pubkeys, hub+stream endpoints)
 *       writes WIFI           (ECIES envelope: SSID + password)
 *       observes STATUS notify (IDLE → APPLYING → SUCCESS/ERROR)
 *
 * Crypto is ECIES v2 (X25519 static-static + ephemeral ECDH → HKDF-SHA256 →
 * AES-256-GCM), identical to the rest of the fleet (hub/hub/crypto.py and the
 * Zephyr device fw_common/hub_crypto.c).  The WiFi password is the only secret
 * and travels encrypted; the endpoints/pubkeys/name are plaintext.
 *
 * Everything provisioned is persisted to NVS (namespace "nnprov" for keys and
 * endpoints; WiFi SSID/pass + stream endpoint go through nn_netstream's own
 * "nnstream" namespace).  On reboot the saved config is re-applied and the
 * stream uplink auto-starts — no re-provisioning needed.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Provisioning status, mirrored on the BLE STATUS characteristic. */
typedef enum {
    NN_PROV_IDLE     = 0x00,
    NN_PROV_APPLYING = 0x01,
    NN_PROV_SUCCESS  = 0x02,
    NN_PROV_ERROR    = 0x03,
} nn_prov_status_t;

/* Initialise crypto (load/generate the device X25519 key), load any saved
 * provisioning config from NVS, and apply it (configure + start the uplink if
 * complete).  Does NOT start BLE advertising — call nn_prov_start() for that. */
esp_err_t nn_prov_init(void);

/* Bring up the BLE GATT provisioning service and start advertising.  Safe to
 * call even if the node is already provisioned (allows re-provisioning). */
esp_err_t nn_prov_start(void);

/* True once a complete, valid config (WiFi + endpoints + keys) has been
 * applied — from this boot's provisioning OR a prior one restored from NVS. */
bool nn_prov_is_provisioned(void);

/* Copy the device's 32-byte X25519 public key (what the hub reads). */
void nn_prov_get_device_pubkey(uint8_t out[32]);

/* Accessors for the provisioned hub control endpoint + peer public keys, for
 * the (future) control channel and stream-encryption consumers.  Return false
 * if that field has not been provisioned yet. */
bool nn_prov_get_hub_endpoint(char *host, size_t host_cap, uint16_t *port);
bool nn_prov_get_hub_pubkey(uint8_t out[32]);
bool nn_prov_get_stream_pubkey(uint8_t out[32]);

/* One-line status into `out` (for the `prov` console command). */
void nn_prov_status_str(char *out, size_t n);

/* Register the `prov` console command. */
void nn_prov_cli_register(void);

#ifdef __cplusplus
}
#endif
