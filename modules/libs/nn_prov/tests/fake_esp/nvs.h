/* SPDX-License-Identifier: Apache-2.0 — host-test fake NVS.
 * Pattern encoded: namespaced key→blob store; get_str/get_blob honour and
 * update the caller's size; open can fail (unprovisioned NVS). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
#define NVS_READONLY  0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out);
void      nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len);
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t v);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out);
