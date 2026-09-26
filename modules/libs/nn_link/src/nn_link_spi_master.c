/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_link SPI master backend — ESP32-P4 SPI master + essl_spi.
 *
 * Talks to the C6 spi_slave_hd (segment mode) via the ESP serial-slave-link
 * over SPI.  Shared-register flow control (see nn_link_spi_slave.c):
 *   TX_READY_SIZE  — cumulative bytes the slave has queued for us to read
 *   RX_READY_NUM   — cumulative RX buffers the slave has ready to receive into
 * A handshake GPIO (D1, input) signals "slave has data to read".
 *
 * Packet boundaries: at CLI data rates the slave rarely has more than one TX
 * transaction outstanding, so each "available bytes" read maps to one packet.
 * (A length-prefix framing can be added later for high-rate streaming.)
 */
#include "nn_link_priv.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_serial_slave_link/essl_spi.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_link_spi_master);
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include <string.h>


#define HOST            SPI2_HOST
/* Quad drives 4 data lines at once over un-pulled jumper wires, so it needs a
 * gentler clock than single-line to stay reliable on the bench. */
#if CONFIG_NN_LINK_SPI_QUAD
#  define SPI_CLOCK_HZ  (2 * 1000 * 1000)    /* 2 MHz */
#elif CONFIG_NN_LINK_SPI_DUAL
#  define SPI_CLOCK_HZ  (8 * 1000 * 1000)    /* 8 MHz — 2-bit over IO0/IO1 */
#else
#  define SPI_CLOCK_HZ  (10 * 1000 * 1000)   /* 10 MHz */
#endif
#define BUF_SIZE        NN_LINK_RAW_MAX       /* framed buffers */

#define REG_READY_FLAG      0
#define REG_MAX_TX_LEN      4
#define REG_MAX_RX_LEN      8
#define REG_TX_READY_SIZE   12
#define REG_RX_READY_NUM    16
#define READY_FLAG          0xEE

/* DMA data phase line mode.  Registers (rdbuf) stay single-line for
 * reliability; only the bulk DMA transfers go quad when enabled. */
#if CONFIG_NN_LINK_SPI_QUAD
#  define DMA_LINE_FLAGS    SPI_TRANS_MODE_QIO
#elif CONFIG_NN_LINK_SPI_DUAL
#  define DMA_LINE_FLAGS    SPI_TRANS_MODE_DIO
#else
#  define DMA_LINE_FLAGS    0
#endif

static spi_device_handle_t s_spi;
static SemaphoreHandle_t   s_lock;     /* serialises essl ops + counters */
static uint8_t            *s_rx_buf;
static uint8_t            *s_tx_buf;
static uint32_t            s_size_has_read;  /* bytes read from slave   */
static uint32_t            s_num_has_sent;   /* RX buffers consumed     */

const char *nn_link_role_str(void) { return "master"; }

/* Read a 4-byte shared register, retrying until two consecutive reads agree
 * (the slave updates it byte-wise).  BOUNDED: give up after a few tries and
 * return the last value — an unbounded spin here starves the idle task and
 * trips the task watchdog when the bus is momentarily noisy. */
static uint32_t read_reg_stable(int addr)
{
    uint32_t a = 0, b = 0;
    essl_spi_rdbuf_polling(s_spi, (uint8_t *)&a, addr, 4, 0);
    for (int i = 0; i < 8; i++) {
        essl_spi_rdbuf_polling(s_spi, (uint8_t *)&b, addr, 4, 0);
        if (a == b) return b;
        a = b;
    }
    return b;
}

esp_err_t nn_link_init(void)
{
    if (!s_lock) { s_lock = xSemaphoreCreateMutex(); if (!s_lock) return ESP_ERR_NO_MEM; }

    spi_bus_config_t bus = {
        .mosi_io_num = NN_LINK_SPI_P4_MOSI,
        .miso_io_num = NN_LINK_SPI_P4_MISO,
        .sclk_io_num = NN_LINK_SPI_P4_SCLK,
#if CONFIG_NN_LINK_SPI_QUAD
        .quadwp_io_num = NN_LINK_SPI_P4_WP,
        .quadhd_io_num = NN_LINK_SPI_P4_HD,
        .flags = SPICOMMON_BUSFLAG_QUAD,
#elif CONFIG_NN_LINK_SPI_DUAL
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .flags = SPICOMMON_BUSFLAG_DUAL,
#else
        .quadwp_io_num = -1, .quadhd_io_num = -1,
#endif
        .max_transfer_sz = BUF_SIZE,
    };
    esp_err_t ret = spi_bus_initialize(HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) { NN_LOG_ERR("bus_init: %s", esp_err_to_name(ret)); return ret; }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = NN_LINK_SPI_P4_CS,
        .command_bits = 8, .address_bits = 8, .dummy_bits = 8,
        .queue_size = 16,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    ret = spi_bus_add_device(HOST, &dev, &s_spi);
    if (ret != ESP_OK) { NN_LOG_ERR("add_device: %s", esp_err_to_name(ret)); return ret; }

#if CONFIG_NN_LINK_SPI_HANDSHAKE
    gpio_config_t hs = { .pin_bit_mask = 1ULL << NN_LINK_SPI_P4_HS,
                         .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE };
    gpio_config(&hs);
#endif

    s_rx_buf = spi_bus_dma_memory_alloc(HOST, BUF_SIZE, 0);
    s_tx_buf = spi_bus_dma_memory_alloc(HOST, BUF_SIZE, 0);
    if (!s_rx_buf || !s_tx_buf) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

/* Backend transmit: send one already-framed buffer to the slave's RX DMA. */
esp_err_t nn_link__raw_send(const uint8_t *frame, size_t len)
{
    if (!frame || len == 0 || len > BUF_SIZE) return ESP_ERR_INVALID_ARG;
    if (!s_spi) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_TIMEOUT;
    /* Wait until the slave has an RX buffer free for us. */
    for (int i = 0; i < 100; i++) {
        if (read_reg_stable(REG_RX_READY_NUM) - s_num_has_sent > 0) {
            memcpy(s_tx_buf, frame, len);
            ret = essl_spi_wrdma(s_spi, s_tx_buf, len, -1, DMA_LINE_FLAGS);
            if (ret == ESP_OK) s_num_has_sent++;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    xSemaphoreGive(s_lock);
    return ret;
}

/* Read at most one queued chunk from the slave and deliver it.  Bounded work
 * per call (no inner busy-loop) so the rx task always yields — and a sanity
 * cap on `avail` so a stale/garbage register read can't spin us forever. */
static void drain_rx(void)
{
    uint8_t local[BUF_SIZE];
    uint32_t n = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t ready = read_reg_stable(REG_TX_READY_SIZE);
    uint32_t avail = ready - s_size_has_read;     /* unsigned wrap-safe */
    if (avail == 0) {                             /* nothing queued */
        xSemaphoreGive(s_lock);
        return;
    }
    if (avail > (16u * BUF_SIZE)) {
        /* Bogus: the peer's cumulative counter went backward (the slave reset
         * it on ITS reboot) or we're otherwise desynced.  Auto-heal by snapping
         * our read cursor to the slave's current size so we read only NEW data
         * from here on, instead of wedging forever. */
        s_size_has_read = ready;
        xSemaphoreGive(s_lock);
        return;
    }
    n = avail > BUF_SIZE ? BUF_SIZE : avail;
    if (essl_spi_rddma(s_spi, s_rx_buf, n, -1, DMA_LINE_FLAGS) == ESP_OK) {
        s_size_has_read += avail;                 /* whole chunk left the bus */
        memcpy(local, s_rx_buf, n);
    } else {
        n = 0;
    }
    xSemaphoreGive(s_lock);

    /* Deframe + deliver OUTSIDE the lock (the rx cb may call nn_link_send). */
    if (n > 0) nn_link__on_raw_rx(local, n);
}

static void rx_task(void *arg)
{
    (void)arg;
    for (;;) {
#if CONFIG_NN_LINK_SPI_HANDSHAKE
        /* Cheap: only touch the bus when the slave raises the handshake. */
        if (gpio_get_level(NN_LINK_SPI_P4_HS)) {
            drain_rx();
        }
#else
        drain_rx();
#endif
        /* Always yield at least one tick (pdMS_TO_TICKS(<10) is 0 at 100 Hz,
         * which would NOT yield and would starve the idle task -> task WDT). */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t nn_link_start(void)
{
    if (nn_link_is_connected()) return ESP_OK;

    /* Wait for the C6 slave to publish its ready flag. */
    NN_LOG_INF("waiting for SPI slave ready flag...");
    int tries = 0;
    while (read_reg_stable(REG_READY_FLAG) != READY_FLAG) {
        if (++tries % 10 == 0) NN_LOG_WRN("slave not ready yet (try %d)...", tries);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    uint32_t max_tx = read_reg_stable(REG_MAX_TX_LEN);
    uint32_t max_rx = read_reg_stable(REG_MAX_RX_LEN);
    NN_LOG_INF("slave ready (max tx=%u rx=%u)", (unsigned)max_tx, (unsigned)max_rx);

    /* Re-sync our read counter to the slave's CURRENT cumulative TX size.  The
     * slave's counters survive across OUR reboot (it stays up); if we assumed 0
     * after a master reboot, avail = TX_READY_SIZE(stale-high) - 0 would look
     * bogus and drain_rx would never read — wedging every slave->master packet
     * (the C6's VERSION_REQ / ACKs / OTA replies).  Starting from the slave's
     * current size means we read only data queued from here on.  (Fresh cold
     * boot: TX_READY_SIZE is 0, so this is a no-op.) */
    s_size_has_read = read_reg_stable(REG_TX_READY_SIZE);
    s_num_has_sent  = 0;   /* RX_READY_NUM is cumulative+large → we can send */

    nn_link__set_connected(true);

    if (xTaskCreate(rx_task, "nn_link_rx", 4096, NULL, 10, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    /* Retry HELLO until the slave ACKs — robust against a lost first packet
     * and against the slave still finishing its own startup. */
    NN_LOG_INF("handshaking (HELLO) with slave...");
    for (int i = 0; i < 20 && !nn_link__ack_seen(); i++) {
        nn_link_send((const uint8_t *)NN_LINK_HELLO, strlen(NN_LINK_HELLO));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return ESP_OK;
}
