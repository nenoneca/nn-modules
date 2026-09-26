/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_pal/wifi.h>

#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <nn_osal/osal.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>

NN_OSAL_LOG_MODULE(nn_pal_wifi);

static struct net_mgmt_event_callback s_cb;
static nn_pal_wifi_cb_t s_user_cb;
static void            *s_user;
static atomic_t         s_connected = ATOMIC_INIT(0);
static nn_pal_wifi_state_t s_state;

static void evt(struct net_mgmt_event_callback *cb, uint64_t event,
                struct net_if *iface)
{
    (void)iface;
    if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
        const struct wifi_status *st = cb->info;
        int s = st ? st->conn_status : -1;
        if (s == WIFI_STATUS_CONN_SUCCESS) {
            atomic_set(&s_connected, 1);
            s_state.disconnect_reason = 0;
            if (s_user_cb) s_user_cb(NN_PAL_WIFI_EV_CONNECTED, &s_state, s_user);
        } else {
            atomic_set(&s_connected, 0);
            s_state.disconnect_reason = s;
            if (s_user_cb) s_user_cb(NN_PAL_WIFI_EV_DISCONNECTED, &s_state, s_user);
        }
    } else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        const struct wifi_status *st = cb->info;
        int reason = st ? st->disconn_reason : -1;
        atomic_set(&s_connected, 0);
        s_state.disconnect_reason = reason;
        if (s_user_cb) s_user_cb(NN_PAL_WIFI_EV_DISCONNECTED, &s_state, s_user);
    }
}

int nn_pal_wifi_init(nn_pal_wifi_cb_t cb, void *user)
{
    s_user_cb = cb;
    s_user    = user;
    net_mgmt_init_event_callback(&s_cb, evt,
                                 NET_EVENT_WIFI_CONNECT_RESULT |
                                 NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&s_cb);
    return 0;
}

int nn_pal_wifi_connect(const char *ssid, const char *psk)
{
    if (!ssid || !psk) return -EINVAL;
    struct net_if *iface = net_if_get_first_wifi();
    if (!iface) return -ENODEV;

    struct wifi_connect_req_params p = {
        .ssid        = (uint8_t *)ssid,
        .ssid_length = strlen(ssid),
        .psk         = (uint8_t *)psk,
        .psk_length  = strlen(psk),
        .channel     = WIFI_CHANNEL_ANY,
        /* The Zephyr ESP32 driver only accepts a narrow set of security
         * types — WPA2-PSK covers the 2.4 GHz home-AP case we provision
         * against. */
        .security    = WIFI_SECURITY_TYPE_PSK,
        .mfp         = WIFI_MFP_OPTIONAL,
        .band        = WIFI_FREQ_BAND_2_4_GHZ,
    };
    NN_LOG_INF("connect \"%s\"", ssid);
    return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p));
}

int nn_pal_wifi_disconnect(void)
{
    struct net_if *iface = net_if_get_first_wifi();
    if (!iface) return -ENODEV;
    return net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
}

int nn_pal_wifi_get_state(nn_pal_wifi_state_t *out)
{
    if (!out) return -EINVAL;
    *out = s_state;
    return 0;
}

bool nn_pal_wifi_is_connected(void)
{
    return atomic_get(&s_connected) != 0;
}
