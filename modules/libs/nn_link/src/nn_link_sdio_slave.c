/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_link slave backend — ESP32-C6 SDIO slave.
 *
 * The C6 is passive: it starts the SDIO slave on its fixed IO_MUX pins
 * (CLK19/CMD18/D0..D3 20..23) and waits for the P4 master to enumerate it
 * and send the HELLO packet.  Each received packet is reassembled and handed
 * to the rx callback; nn_link_send() pushes a packet back to the host.
 */
#include "nn_link_priv.h"
#include "driver/sdio_slave.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_link_sdio_slave);
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>


#define RECV_BUFSIZE   256          /* must match sdio_slave recv_buffer_size */
#define RECV_BUFS      8            /* up to RECV_BUFS*RECV_BUFSIZE in flight  */
#define SEND_BUFS      4
#define SEND_QUEUE_SZ  (SEND_BUFS + 1)

/* recv buffers owned by the driver */
DMA_ATTR static uint8_t s_recv_buf[RECV_BUFS][RECV_BUFSIZE];
/* send buffer pool (DMA-capable, word aligned) + free-index queue */
DMA_ATTR static uint8_t s_send_buf[SEND_BUFS][NN_LINK_RAW_MAX];
static QueueHandle_t s_send_free;   /* holds free indices [0..SEND_BUFS) */

/* reassembly scratch for a full multi-buffer packet */
static uint8_t s_packet[NN_LINK_RAW_MAX];

const char *nn_link_role_str(void) { return "slave"; }

esp_err_t nn_link_init(void)
{
    sdio_slave_config_t config = {
        .sending_mode     = SDIO_SLAVE_SEND_PACKET,
        .send_queue_size  = SEND_QUEUE_SZ,
        .recv_buffer_size = RECV_BUFSIZE,
        .event_cb         = NULL,
        /* Internal pull-ups help bring-up on bare devkit wiring.  External
         * 10k pull-ups on CMD/D0..D3 are recommended for a real board. */
        .flags            = SDIO_SLAVE_FLAG_INTERNAL_PULLUP,
    };

    esp_err_t ret = sdio_slave_initialize(&config);
    if (ret != ESP_OK) {
        NN_LOG_ERR("sdio_slave_initialize: %s", esp_err_to_name(ret));
        return ret;
    }

    for (int i = 0; i < RECV_BUFS; i++) {
        sdio_slave_buf_handle_t h = sdio_slave_recv_register_buf(s_recv_buf[i]);
        if (!h) {
            NN_LOG_ERR("recv_register_buf %d failed", i);
            return ESP_ERR_NO_MEM;
        }
        ret = sdio_slave_recv_load_buf(h);
        if (ret != ESP_OK) return ret;
    }

    sdio_slave_set_host_intena(SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET);

    s_send_free = xQueueCreate(SEND_BUFS, sizeof(int));
    for (int i = 0; i < SEND_BUFS; i++) {
        xQueueSend(s_send_free, &i, 0);
    }
    return ESP_OK;
}

/* Reclaim send buffers the driver has finished transmitting. */
static void reclaim_finished(void)
{
    for (;;) {
        void *arg = NULL;
        if (sdio_slave_send_get_finished(&arg, 0) != ESP_OK) {
            break;
        }
        int idx = (int)(intptr_t)arg;
        xQueueSend(s_send_free, &idx, 0);
    }
}

esp_err_t nn_link__raw_send(const uint8_t *frame, size_t len)
{
    if (!frame || len == 0 || len > NN_LINK_RAW_MAX) return ESP_ERR_INVALID_ARG;

    reclaim_finished();
    int idx;
    if (xQueueReceive(s_send_free, &idx, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;     /* all send buffers busy */
    }
    memcpy(s_send_buf[idx], frame, len);
    esp_err_t ret = sdio_slave_send_queue(s_send_buf[idx], len,
                                          (void *)(intptr_t)idx,
                                          pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) {
        xQueueSend(s_send_free, &idx, 0);   /* give the buffer back */
    }
    return ret;
}

/* Pull one whole packet (possibly spanning several driver buffers) into
 * s_packet; returns length or -1 on timeout/none. */
static int recv_one_packet(TickType_t first_wait)
{
    sdio_slave_buf_handle_t handle;
    esp_err_t ret = sdio_slave_recv_packet(&handle, first_wait);
    if (ret == ESP_ERR_TIMEOUT) return -1;

    int total = 0;
    bool more = (ret == ESP_ERR_NOT_FINISHED);
    for (;;) {
        size_t len = 0;
        uint8_t *ptr = sdio_slave_recv_get_buf(handle, &len);
        if (ptr && total + (int)len <= (int)sizeof s_packet) {
            memcpy(s_packet + total, ptr, len);
            total += len;
        }
        /* buffer consumed → hand it back to the driver */
        sdio_slave_recv_load_buf(handle);
        if (!more) break;
        ret = sdio_slave_recv_packet(&handle, portMAX_DELAY);
        more = (ret == ESP_ERR_NOT_FINISHED);
    }
    return total;
}

static void rx_task(void *arg)
{
    (void)arg;
    NN_LOG_INF("slave ready, waiting for P4 master...");
    for (;;) {
        int n = recv_one_packet(pdMS_TO_TICKS(100));
        if (n > 0) {
            nn_link__on_raw_rx(s_packet, n);    /* deframe + handshake + deliver */
        }
        reclaim_finished();
    }
}

esp_err_t nn_link_start(void)
{
    esp_err_t ret = sdio_slave_start();
    if (ret != ESP_OK) {
        NN_LOG_ERR("sdio_slave_start: %s", esp_err_to_name(ret));
        return ret;
    }
    if (xTaskCreate(rx_task, "nn_link_rx", 4096, NULL, 10, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
