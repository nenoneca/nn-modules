/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * nn_pal/wifi.h — STA bring-up.
 *
 * Only the ncp_host gateway uses WiFi (to reach the hub over the
 * infrastructure network).  Sensors are Thread-only.  This surface is
 * therefore tiny: configure SSID/PSK, connect, watch events, read RSSI.
 *
 * AP mode and the broader esp_wifi/wifi_mgmt configuration space are
 * deliberately not exposed.  If a future use case needs them, extend
 * the API rather than reaching past the PAL.
 */

typedef enum {
    NN_PAL_WIFI_EV_CONNECTED    = 1,
    NN_PAL_WIFI_EV_DISCONNECTED = 2,
    NN_PAL_WIFI_EV_IP_ASSIGNED  = 3,
} nn_pal_wifi_event_t;

typedef struct {
    /* Holds the IPv4 address as `a.b.c.d` if known, else "" */
    char ipv4[16];
    /* Last RSSI value or 0 if unknown */
    int8_t rssi_dbm;
    /* Reason code from the platform on disconnect; 0 otherwise */
    int    disconnect_reason;
} nn_pal_wifi_state_t;

/* Callback signature.  `user` is the opaque pointer passed at init. */
typedef void (*nn_pal_wifi_cb_t)(nn_pal_wifi_event_t ev,
                                 const nn_pal_wifi_state_t *st,
                                 void *user);

int nn_pal_wifi_init(nn_pal_wifi_cb_t cb, void *user);

/* Connect to the configured AP.  If the chip is already connected,
 * this returns 0 immediately. */
int nn_pal_wifi_connect(const char *ssid, const char *psk);

int nn_pal_wifi_disconnect(void);

/* Snapshot of current state (ip + rssi). */
int nn_pal_wifi_get_state(nn_pal_wifi_state_t *out);

bool nn_pal_wifi_is_connected(void);
