/* SPDX-License-Identifier: Apache-2.0 */
/* nn_pal/wifi.h — ESP-IDF STA backend.  Lifts the esp_wifi/esp_netif STA
 * bring-up out of nn_netstream behind the PAL surface, preserving the exact
 * behaviour it had: auto-(re)connect on STA_START / STA_DISCONNECTED, and
 * WIFI_PS_NONE (the modem-sleep-off that keeps the video uplink at full
 * throughput — part of the fps fix). */
#if defined(CONFIG_NN_PAL_BACKEND_ESP_IDF)

#include <nn_pal/wifi.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static nn_pal_wifi_cb_t s_cb;
static void            *s_user;
static volatile bool    s_connected;     /* has an IP (usable) */
static char             s_ipv4[16];
static bool             s_started;

static void emit(nn_pal_wifi_event_t ev)
{
    if (!s_cb) return;
    nn_pal_wifi_state_t st = { .rssi_dbm = 0, .disconnect_reason = 0 };
    strlcpy(st.ipv4, s_ipv4, sizeof st.ipv4);
    s_cb(ev, &st, s_user);
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ipv4[0] = '\0';
        emit(NN_PAL_WIFI_EV_DISCONNECTED);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ipv4, sizeof s_ipv4, IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        emit(NN_PAL_WIFI_EV_IP_ASSIGNED);
    }
}

int nn_pal_wifi_init(nn_pal_wifi_cb_t cb, void *user)
{
    s_cb = cb;
    s_user = user;
    return 0;
}

int nn_pal_wifi_connect(const char *ssid, const char *psk)
{
    if (!ssid || ssid[0] == '\0') return -1;
    if (s_connected) return 0;
    if (!s_started) {
        if (esp_netif_init() != ESP_OK) return -1;
        if (esp_event_loop_create_default() != ESP_OK) return -1;
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&ic) != ESP_OK) return -1;
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL);
        s_started = true;
    }
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, psk ? psk : "", sizeof wc.sta.password);
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return -1;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return -1;
    if (esp_wifi_start() != ESP_OK) return -1;
    /* Keep the radio always-on for the continuous video uplink (fps fix). */
    esp_wifi_set_ps(WIFI_PS_NONE);
    return 0;
}

int nn_pal_wifi_disconnect(void)
{
    s_connected = false;
    return esp_wifi_disconnect() == ESP_OK ? 0 : -1;
}

int nn_pal_wifi_get_state(nn_pal_wifi_state_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    strlcpy(out->ipv4, s_ipv4, sizeof out->ipv4);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi_dbm = ap.rssi;
    return 0;
}

bool nn_pal_wifi_is_connected(void)
{
    return s_connected;
}

#endif /* CONFIG_NN_PAL_BACKEND_ESP_IDF */
