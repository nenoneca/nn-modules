/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_link SPI slave backend — ESP32-C6 spi_slave_hd (segment mode).
 *
 * Robust, breadboard-friendly alternative to the SDIO slave: push-pull SPI on
 * GPIO-matrix pins (no open-drain pull-up bus).  Mirrors the IDF
 * spi_slave_hd/segment_mode example, wrapped in nn_link's packet API.
 *
 * Master/slave flow control via shared registers (CONF buffer):
 *   REG0  READY_FLAG (0xEE once slave is up)
 *   REG4  MAX_TX_BUF_LEN          REG8  MAX_RX_BUF_LEN
 *   REG12 TX_READY_BUF_SIZE (cumulative bytes slave has queued for master)
 *   REG16 RX_READY_BUF_NUM  (cumulative RX buffers slave has loaded)
 * A handshake GPIO (D1) is raised while the slave has unread TX data.
 */
#include "nn_link_priv.h"
#include "driver/spi_slave_hd.h"
#include "driver/gpio.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_link_spi_slave);
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#include <string.h>


#define HOST            SPI2_HOST
#define QUEUE_SIZE      2       /* was 4; halved to shrink the DMA-buffer pool
                                 * (2*BUF_SIZE tx + 2*rx) — the ~8KB alloc was
                                 * failing NO_MEM at nn_link_init on the C6's
                                 * fragmented DMA-capable heap on cold boot. */
#define BUF_SIZE        NN_LINK_RAW_MAX       /* framed buffers */

#define REG_READY_FLAG      0
#define REG_MAX_TX_LEN      4
#define REG_MAX_RX_LEN      8
#define REG_TX_READY_SIZE   12
#define REG_RX_READY_NUM    16
#define READY_FLAG          0xEE

static uint32_t s_tx_ready_size;   /* cumulative bytes queued for master */
static uint32_t s_rx_ready_num;    /* cumulative RX buffers loaded       */

/* TX descriptor pool + free-index queue (producer: nn_link_send) */
static uint8_t      *s_tx_buf[QUEUE_SIZE];
static spi_slave_hd_data_t s_tx_trans[QUEUE_SIZE];
static QueueHandle_t s_tx_free;
static volatile int  s_tx_outstanding;

const char *nn_link_role_str(void) { return "slave"; }

/* Driver callbacks: publish loaded counters to the shared registers.
 * (Not IRAM — they call spi_slave_hd_write_buffer, matching the IDF example.) */
static bool cb_tx_ready(void *arg, spi_slave_hd_event_t *ev, BaseType_t *awoken)
{
    s_tx_ready_size += ev->trans->len;
    spi_slave_hd_write_buffer(HOST, REG_TX_READY_SIZE, (uint8_t *)&s_tx_ready_size, 4);
    return true;
}
static bool cb_rx_ready(void *arg, spi_slave_hd_event_t *ev, BaseType_t *awoken)
{
    s_rx_ready_num++;
    spi_slave_hd_write_buffer(HOST, REG_RX_READY_NUM, (uint8_t *)&s_rx_ready_num, 4);
    return true;
}

#if CONFIG_NN_LINK_SPI_HANDSHAKE
static void hs_set(int level) { gpio_set_level(NN_LINK_SPI_C6_HS, level); }
#else
static void hs_set(int level) { (void)level; }
#endif

esp_err_t nn_link_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = NN_LINK_SPI_C6_MOSI,
        .miso_io_num = NN_LINK_SPI_C6_MISO,
        .sclk_io_num = NN_LINK_SPI_C6_SCLK,
#if CONFIG_NN_LINK_SPI_QUAD
        .quadwp_io_num = NN_LINK_SPI_C6_WP,
        .quadhd_io_num = NN_LINK_SPI_C6_HD,
        .flags = SPICOMMON_BUSFLAG_QUAD,
#elif CONFIG_NN_LINK_SPI_DUAL
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .flags = SPICOMMON_BUSFLAG_DUAL,
#else
        .quadwp_io_num = -1, .quadhd_io_num = -1,
#endif
        .max_transfer_sz = BUF_SIZE,
    };
    spi_slave_hd_slot_config_t cfg = {
        .spics_io_num = NN_LINK_SPI_C6_CS,
        .mode = 0,
        .command_bits = 8, .address_bits = 8, .dummy_bits = 8,
        .queue_size = QUEUE_SIZE,
        .dma_chan = SPI_DMA_CH_AUTO,
        .cb_config = {
            .cb_send_dma_ready = cb_tx_ready,
            .cb_recv_dma_ready = cb_rx_ready,
        },
    };
    esp_err_t ret = spi_slave_hd_init(HOST, &bus, &cfg);
    if (ret != ESP_OK) { NN_LOG_ERR("spi_slave_hd_init: %s", esp_err_to_name(ret)); return ret; }

#if CONFIG_NN_LINK_SPI_HANDSHAKE
    gpio_config_t hs = { .pin_bit_mask = 1ULL << NN_LINK_SPI_C6_HS,
                         .mode = GPIO_MODE_OUTPUT };
    gpio_config(&hs);
    hs_set(0);
#endif

    /* TX pool */
    s_tx_free = xQueueCreate(QUEUE_SIZE, sizeof(int));
    for (int i = 0; i < QUEUE_SIZE; i++) {
        s_tx_buf[i] = heap_caps_calloc(1, BUF_SIZE, MALLOC_CAP_DMA);
        if (!s_tx_buf[i]) return ESP_ERR_NO_MEM;
        xQueueSend(s_tx_free, &i, 0);
    }

    /* Publish capabilities + ready flag. */
    uint32_t cap = BUF_SIZE;
    spi_slave_hd_write_buffer(HOST, REG_MAX_TX_LEN, (uint8_t *)&cap, 4);
    spi_slave_hd_write_buffer(HOST, REG_MAX_RX_LEN, (uint8_t *)&cap, 4);
    uint32_t zero = 0;
    spi_slave_hd_write_buffer(HOST, REG_TX_READY_SIZE, (uint8_t *)&zero, 4);
    spi_slave_hd_write_buffer(HOST, REG_RX_READY_NUM, (uint8_t *)&zero, 4);
    return ESP_OK;
}

/* Backend transmit: queue one already-framed buffer to the host. */
esp_err_t nn_link__raw_send(const uint8_t *frame, size_t len)
{
    if (!frame || len == 0 || len > BUF_SIZE) return ESP_ERR_INVALID_ARG;

    int idx;
    if (xQueueReceive(s_tx_free, &idx, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(s_tx_buf[idx], frame, len);
    s_tx_trans[idx].data = s_tx_buf[idx];
    s_tx_trans[idx].len  = len;
    s_tx_trans[idx].flags = SPI_SLAVE_HD_TRANS_DMA_BUFFER_ALIGN_AUTO;
    s_tx_trans[idx].arg  = (void *)(intptr_t)idx;
    esp_err_t ret = spi_slave_hd_queue_trans(HOST, SPI_SLAVE_CHAN_TX,
                                             &s_tx_trans[idx], pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) { xQueueSend(s_tx_free, &idx, 0); return ret; }
    s_tx_outstanding++;
    hs_set(1);                 /* tell the master we have data to read */
    return ESP_OK;
}

/* Recycle finished TX descriptors; lower the handshake when fully drained. */
static void tx_recycle_task(void *arg)
{
    (void)arg;
    for (;;) {
        spi_slave_hd_data_t *done = NULL;
        if (spi_slave_hd_get_trans_res(HOST, SPI_SLAVE_CHAN_TX, &done,
                                       portMAX_DELAY) == ESP_OK && done) {
            int idx = (int)(intptr_t)done->arg;
            xQueueSend(s_tx_free, &idx, 0);
            if (--s_tx_outstanding <= 0) {
                s_tx_outstanding = 0;
                hs_set(0);
            }
        }
    }
}

static uint8_t            *s_rx_buf[QUEUE_SIZE];
static spi_slave_hd_data_t s_rx_trans[QUEUE_SIZE];

/* Allocate + pre-queue the RX buffers.  Called SYNCHRONOUSLY before the ready
 * flag is raised, so RX_READY_NUM is already populated when the master sends
 * its first packet (otherwise the master's HELLO races an empty RX queue). */
static esp_err_t prequeue_rx(void)
{
    for (int i = 0; i < QUEUE_SIZE; i++) {
        s_rx_buf[i] = heap_caps_calloc(1, BUF_SIZE, MALLOC_CAP_DMA);
        if (!s_rx_buf[i]) return ESP_ERR_NO_MEM;
        s_rx_trans[i].data = s_rx_buf[i];
        s_rx_trans[i].len  = BUF_SIZE;
        s_rx_trans[i].flags = SPI_SLAVE_HD_TRANS_DMA_BUFFER_ALIGN_AUTO;
        esp_err_t ret = spi_slave_hd_queue_trans(HOST, SPI_SLAVE_CHAN_RX,
                                                 &s_rx_trans[i], portMAX_DELAY);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

/* Deliver received packets and re-queue buffers. */
static void rx_task(void *arg)
{
    (void)arg;
    NN_LOG_INF("SPI slave ready, waiting for P4 master...");
    int id = 0;
    for (;;) {
        spi_slave_hd_data_t *done = NULL;
        if (spi_slave_hd_get_trans_res(HOST, SPI_SLAVE_CHAN_RX, &done,
                                       portMAX_DELAY) != ESP_OK || !done) {
            continue;
        }
        size_t n = done->trans_len;
        uint8_t *p = done->data;
        if (n > 0) {
            nn_link__on_raw_rx(p, n);   /* deframe + handshake + deliver */
        }
        /* re-queue this buffer */
        s_rx_trans[id].data = done->data;
        s_rx_trans[id].len  = BUF_SIZE;
        s_rx_trans[id].flags = SPI_SLAVE_HD_TRANS_DMA_BUFFER_ALIGN_AUTO;
        ESP_ERROR_CHECK(spi_slave_hd_queue_trans(HOST, SPI_SLAVE_CHAN_RX,
                                                 &s_rx_trans[id], portMAX_DELAY));
        id = (id + 1) % QUEUE_SIZE;
    }
}

esp_err_t nn_link_start(void)
{
    /* Pre-queue RX buffers BEFORE raising the ready flag so RX_READY_NUM is
     * already valid when the master sends its first (HELLO) packet. */
    esp_err_t ret = prequeue_rx();
    if (ret != ESP_OK) { NN_LOG_ERR("prequeue_rx: %s", esp_err_to_name(ret)); return ret; }

    if (xTaskCreate(rx_task, "nn_link_rx", 4096, NULL, 10, NULL) != pdPASS ||
        xTaskCreate(tx_recycle_task, "nn_link_tx", 3072, NULL, 9, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    /* Mark the slave ready LAST so the master only proceeds once buffers
     * are queued. */
    uint32_t flag = READY_FLAG;
    spi_slave_hd_write_buffer(HOST, REG_READY_FLAG, (uint8_t *)&flag, 4);
    return ESP_OK;
}
