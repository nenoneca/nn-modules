/* SPDX-License-Identifier: Apache-2.0 — test control for the fakes */
#pragma once
#include <stddef.h>
#include <stdint.h>
void fake_nvs_reset(void);
const uint8_t *fake_nvs_get(const char *key, size_t *len);
extern int fake_nvs_writes;
extern char fake_stream_host[64]; extern uint16_t fake_stream_port;
extern uint8_t fake_stream_key[32]; extern uint8_t fake_hub_pub_set[32];
extern char fake_wifi_ssid[33], fake_wifi_pass[65];
extern int fake_statuses[16]; extern int fake_status_count;
extern int fake_restarts, fake_tasks;
extern int fake_crypto_ready; extern int fake_decrypt_rc;
extern uint8_t fake_decrypt_plain[128]; extern size_t fake_decrypt_len;
