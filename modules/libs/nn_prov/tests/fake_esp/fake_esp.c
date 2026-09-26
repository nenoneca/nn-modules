/* SPDX-License-Identifier: Apache-2.0 */
/* Host-test fakes for nn_prov: in-memory NVS + recorders for every
 * side effect the CONFIG/WIFI handlers produce. */
#include "nvs.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "nn_netstream/nn_netstream.h"
#include "nn_prov/nn_prov.h"
#include "fake_esp_ctl.h"
#include <string.h>
#include <stdio.h>

/* — in-memory NVS — */
typedef struct { char key[24]; uint8_t val[600]; size_t len; int used; } slot_t;
static slot_t s_nvs[32];
int fake_nvs_writes;

void fake_nvs_reset(void) { memset(s_nvs, 0, sizeof s_nvs); fake_nvs_writes = 0; }

static slot_t *find(const char *key, int create)
{
    for (int i = 0; i < 32; i++)
        if (s_nvs[i].used && strcmp(s_nvs[i].key, key) == 0) return &s_nvs[i];
    if (!create) return 0;
    for (int i = 0; i < 32; i++)
        if (!s_nvs[i].used) {
            s_nvs[i].used = 1;
            snprintf(s_nvs[i].key, sizeof s_nvs[i].key, "%s", key);
            return &s_nvs[i];
        }
    return 0;
}

const uint8_t *fake_nvs_get(const char *key, size_t *len)
{
    slot_t *s = find(key, 0);
    if (!s) return 0;
    if (len) *len = s->len;
    return s->val;
}

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out)
{ (void)ns; (void)mode; *out = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

static esp_err_t setv(const char *key, const void *val, size_t len)
{
    slot_t *s = find(key, 1);
    if (!s || len > sizeof s->val) return ESP_FAIL;
    memcpy(s->val, val, len); s->len = len; fake_nvs_writes++;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{ (void)h; return setv(k, v, strlen(v) + 1); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t n)
{ (void)h; return setv(k, v, n); }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v)
{ (void)h; return setv(k, &v, 2); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v)
{ (void)h; return setv(k, &v, 1); }

static esp_err_t getv(const char *key, void *out, size_t *len)
{
    slot_t *s = find(key, 0);
    if (!s) return ESP_FAIL;
    if (*len < s->len) return ESP_FAIL;
    memcpy(out, s->val, s->len); *len = s->len;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *o, size_t *n)
{ (void)h; return getv(k, o, n); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *o, size_t *n)
{ (void)h; return getv(k, o, n); }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *o)
{ (void)h; size_t n = 2; return getv(k, o, &n); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *o)
{ (void)h; size_t n = 1; return getv(k, o, &n); }

/* — recorders — */
char fake_stream_host[64]; uint16_t fake_stream_port;
uint8_t fake_stream_key[32]; uint8_t fake_hub_pub_set[32];
char fake_wifi_ssid[33], fake_wifi_pass[65];
int fake_statuses[16]; int fake_status_count;
int fake_restarts, fake_tasks;
int fake_crypto_ready; int fake_decrypt_rc;
uint8_t fake_decrypt_plain[128]; size_t fake_decrypt_len;

void nn_netstream_set_host(const char *host, uint16_t port)
{ snprintf(fake_stream_host, sizeof fake_stream_host, "%s", host);
  fake_stream_port = port; }
void nn_netstream_set_stream_key(const uint8_t k[32])
{ memcpy(fake_stream_key, k, 32); }
void nn_netstream_set_wifi(const char *ssid, const char *pass)
{ snprintf(fake_wifi_ssid, sizeof fake_wifi_ssid, "%s", ssid);
  snprintf(fake_wifi_pass, sizeof fake_wifi_pass, "%s", pass); }

void nn_prov_ble_notify_status(nn_prov_status_t st)
{ if (fake_status_count < 16) fake_statuses[fake_status_count++] = (int)st; }
esp_err_t nn_prov_ble_start(void) { return ESP_OK; }

esp_err_t nn_prov_crypto_init(void) { return ESP_OK; }
void nn_prov_crypto_device_pub(uint8_t out[32]) { memset(out, 0x11, 32); }
void nn_prov_crypto_set_hub_pub(const uint8_t p[32])
{ memcpy(fake_hub_pub_set, p, 32); fake_crypto_ready = 1; }
int  nn_prov_crypto_ready(void) { return fake_crypto_ready; }
esp_err_t nn_prov_crypto_decrypt_h2d(const char *json, size_t json_len,
                                     uint8_t *plain, size_t *plen)
{
    (void)json; (void)json_len;
    if (fake_decrypt_rc != ESP_OK) return fake_decrypt_rc;
    if (*plen < fake_decrypt_len) return ESP_FAIL;
    memcpy(plain, fake_decrypt_plain, fake_decrypt_len);
    *plen = fake_decrypt_len;
    return ESP_OK;
}

void esp_restart(void) { fake_restarts++; }
void vTaskDelay(int t) { (void)t; }
int xTaskCreate(TaskFunction_t fn, const char *name, int stack,
                void *arg, int prio, void *handle)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)handle;
  fake_tasks++; return 1; }

size_t strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (cap) { size_t c = n >= cap ? cap - 1 : n; memcpy(dst, src, c); dst[c] = 0; }
    return n;
}

/* ── nn_osal fakes (nn_prov moved off FreeRTOS/esp_system) ───────────── */
#include "nn_osal/thread.h"
#include "nn_osal/system.h"
#include "nn_osal/time.h"

/* Record the request; never run the entry — nn_prov's only thread reboots. */
int nn_osal_thread_create(nn_osal_thread_t *thread, void *stack,
                          size_t stack_size, nn_osal_thread_entry_t entry,
                          void *a, void *b, void *c,
                          int prio, const char *name)
{
    (void)thread; (void)stack; (void)stack_size; (void)entry;
    (void)a; (void)b; (void)c; (void)prio; (void)name;
    fake_tasks++;
    return 0;
}

void nn_osal_sys_reboot(nn_osal_reboot_type_t type)
{
    (void)type;
    fake_restarts++;
}

uint32_t nn_osal_sleep_ms(uint32_t ms) { (void)ms; return 0; }
