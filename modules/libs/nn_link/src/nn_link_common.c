/* SPDX-License-Identifier: Apache-2.0 */
#include "nn_link_priv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_link);
#include <string.h>


#define BIT_CONNECTED  BIT0

static EventGroupHandle_t s_events;
static nn_link_rx_cb_t    s_rx_cb;
static void              *s_rx_ctx;
static volatile bool      s_connected;
static volatile bool      s_ack_seen;

static EventGroupHandle_t events(void)
{
    /* Lazily created so callers may register an rx cb before nn_link_init(). */
    if (!s_events) {
        s_events = xEventGroupCreate();
    }
    return s_events;
}

void nn_link_set_rx_cb(nn_link_rx_cb_t cb, void *ctx)
{
    s_rx_cb = cb;
    s_rx_ctx = ctx;
}

void nn_link__deliver_rx(const uint8_t *data, size_t len)
{
    nn_link_rx_cb_t cb = s_rx_cb;
    if (cb) {
        cb(data, len, s_rx_ctx);
    }
}

void nn_link__set_connected(bool up)
{
    bool was = s_connected;
    s_connected = up;
    if (up) {
        xEventGroupSetBits(events(), BIT_CONNECTED);
        if (!was) {
            NN_LOG_INF("link CONNECTED (%s)", nn_link_role_str());
        }
    } else {
        xEventGroupClearBits(events(), BIT_CONNECTED);
        if (was) {
            NN_LOG_WRN("link DOWN (%s)", nn_link_role_str());
        }
    }
}

bool nn_link_is_connected(void)
{
    return s_connected;
}

esp_err_t nn_link_wait_connected(int timeout_ms)
{
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY
                                        : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(events(), BIT_CONNECTED,
                                           pdFALSE, pdTRUE, ticks);
    return (bits & BIT_CONNECTED) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool nn_link__ack_seen(void) { return s_ack_seen; }

/* ── Framing layer ───────────────────────────────────────────────────────
 * nn_link_send() frames the payload and hands it to the backend transmit;
 * nn_link__on_raw_rx() deframes a received chunk, runs the link handshake,
 * and delivers the payload to the rx callback. */

esp_err_t nn_link_send(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > NN_LINK_MAX_PACKET) return ESP_ERR_INVALID_ARG;
    if (!s_connected) return ESP_ERR_INVALID_STATE;

    uint8_t frame[NN_LINK_RAW_MAX];
    size_t flen = NN_LINK_FRAME_LEN(len);
    frame[0] = (uint8_t)(len & 0xFF);
    frame[1] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(frame + NN_LINK_HDR_LEN, data, len);
    /* zero the pad so trailing bytes are deterministic */
    memset(frame + NN_LINK_HDR_LEN + len, 0, flen - NN_LINK_HDR_LEN - len);
    return nn_link__raw_send(frame, flen);
}

void nn_link__on_raw_rx(const uint8_t *raw, size_t len)
{
    if (len < NN_LINK_HDR_LEN) return;
    uint16_t plen = (uint16_t)(raw[0] | (raw[1] << 8));
    if (plen == 0 || plen > len - NN_LINK_HDR_LEN) return;   /* bad frame */
    const uint8_t *payload = raw + NN_LINK_HDR_LEN;

    if (!s_connected) nn_link__set_connected(true);

    if (plen == strlen(NN_LINK_HELLO) && memcmp(payload, NN_LINK_HELLO, plen) == 0) {
        /* slave side: answer the master's HELLO */
        NN_LOG_INF("HELLO from master -> ACK");
        nn_link_send((const uint8_t *)NN_LINK_ACK, strlen(NN_LINK_ACK));
    } else if (plen == strlen(NN_LINK_ACK) && memcmp(payload, NN_LINK_ACK, plen) == 0) {
        if (!s_ack_seen) NN_LOG_INF("ACK from slave — handshake complete");
        s_ack_seen = true;
    } else {
        nn_link__deliver_rx(payload, plen);
    }
}
