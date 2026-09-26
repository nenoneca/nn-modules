/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_link master backend — ESP32-P4 SDMMC host driving the C6 SDIO slave.
 *
 * The P4 initiates the link: it enumerates the C6 as an SDIO card on the
 * GPIO-matrix pins (CLK2/CMD3/D0..D3 15..18), brings up the serial-slave-link
 * (ESSL) service, then sends HELLO and waits for the slave's ACK.  A rx task
 * services the slave's "new packet" interrupts and forwards payloads to the
 * rx callback.
 */
#include "nn_link_priv.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_serial_slave_link/essl_sdio.h"
#include "sdmmc_cmd.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_link_sdio_master);
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>


/* P4 host pins (GPIO matrix) — must match the C6 slave wiring.  From the board
 * profile (nn_registry Kconfig, NN_BOARD_*); defaults are the combo harness. */
#define PIN_CLK   CONFIG_NN_LINK_SDIO_CLK
#define PIN_CMD   CONFIG_NN_LINK_SDIO_CMD
#define PIN_D0    CONFIG_NN_LINK_SDIO_D0
#define PIN_D1    CONFIG_NN_LINK_SDIO_D1
#define PIN_D2    CONFIG_NN_LINK_SDIO_D2
#define PIN_D3    CONFIG_NN_LINK_SDIO_D3

#define SLAVE_RECV_BUFSIZE  256     /* must match the slave's recv_buffer_size */
#define TIMEOUT_MAX         UINT32_MAX

static essl_handle_t      s_essl;
static sdmmc_card_t      *s_card;
static SemaphoreHandle_t  s_send_lock;
DRAM_DMA_ALIGNED_ATTR static uint8_t s_send_buf[NN_LINK_RAW_MAX];
DMA_ATTR                static uint8_t s_recv_buf[NN_LINK_RAW_MAX];

const char *nn_link_role_str(void) { return "master"; }

esp_err_t nn_link_init(void)
{
    if (!s_send_lock) {
        s_send_lock = xSemaphoreCreateMutex();
        if (!s_send_lock) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void pullup_pin(int pin)
{
    gpio_pullup_en(pin);
    gpio_pulldown_dis(pin);
}

static esp_err_t enumerate_slave(void)
{
    sdmmc_host_t config = SDMMC_HOST_DEFAULT();
    /* Match Espressif's validated P4 SDIO-host config: HIGH-SPEED + 4-bit +
     * input delay phase 2.  The delay phase (which only takes effect at
     * HIGHSPEED/52M) is what lets the P4 host correctly sample the slave's
     * DAT lines — without it CMD53 block transfers time out even though
     * enumeration (CMD52) succeeds. */
    config.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    config.max_freq_khz = SDMMC_FREQ_HIGHSPEED;     /* 40 MHz */
    config.input_delay_phase = SDMMC_DELAY_PHASE_2;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = PIN_CLK;
    slot.cmd = PIN_CMD;
    slot.d0  = PIN_D0;
    slot.d1  = PIN_D1;
    slot.d2  = PIN_D2;
    slot.d3  = PIN_D3;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;  /* bring-up aid */

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) { NN_LOG_ERR("host_init: %s", esp_err_to_name(err)); return err; }
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    if (err != ESP_OK) { NN_LOG_ERR("init_slot: %s", esp_err_to_name(err)); return err; }

    s_card = calloc(1, sizeof(sdmmc_card_t));
    if (!s_card) return ESP_ERR_NO_MEM;

    int tries = 0;
    while (sdmmc_card_init(&config, s_card) != ESP_OK) {
        if (++tries % 10 == 0) {
            NN_LOG_WRN("slave not responding yet (try %d)...", tries);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    sdmmc_card_print_info(stdout, s_card);

    pullup_pin(PIN_CMD); pullup_pin(PIN_CLK);
    pullup_pin(PIN_D0);  pullup_pin(PIN_D1);
    pullup_pin(PIN_D2);  pullup_pin(PIN_D3);

    essl_sdio_config_t scfg = { .card = s_card, .recv_buffer_size = SLAVE_RECV_BUFSIZE };
    err = essl_sdio_init_dev(&s_essl, &scfg);
    if (err != ESP_OK) { NN_LOG_ERR("essl_init_dev: %s", esp_err_to_name(err)); return err; }

    err = essl_init(s_essl, TIMEOUT_MAX);
    if (err != ESP_OK) { NN_LOG_ERR("essl_init: %s", esp_err_to_name(err)); return err; }

    err = essl_wait_for_ready(s_essl, TIMEOUT_MAX);
    if (err != ESP_OK) { NN_LOG_ERR("wait_for_ready: %s", esp_err_to_name(err)); return err; }
    return ESP_OK;
}

esp_err_t nn_link__raw_send(const uint8_t *frame, size_t len)
{
    if (!frame || len == 0 || len > NN_LINK_RAW_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_essl) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    memcpy(s_send_buf, frame, len);             /* DMA-aligned staging buffer */
    esp_err_t ret = essl_send_packet(s_essl, s_send_buf, len, pdMS_TO_TICKS(1000));
    xSemaphoreGive(s_send_lock);
    return ret;
}

/* Read every packet the slave currently has queued, until the FIFO is empty.
 * Only called when the new-packet interrupt bit is actually set, so a normal
 * "no data" exit is one NOT_FOUND, not a storm of CMD timeouts. */
static void drain_packets(void)
{
    for (;;) {
        size_t got = 0;
        esp_err_t ret = essl_get_packet(s_essl, s_recv_buf, sizeof s_recv_buf,
                                        &got, pdMS_TO_TICKS(200));
        if (ret == ESP_ERR_NOT_FOUND || ret == ESP_ERR_TIMEOUT) {
            break;                                     /* nothing (more) pending */
        }
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
            NN_LOG_WRN("get_packet: %s", esp_err_to_name(ret));
            break;
        }
        if (got > 0) {
            nn_link__on_raw_rx(s_recv_buf, got);   /* deframe + handshake + deliver */
        }
        if (ret == ESP_OK) break;                      /* packet fully read */
    }
}

/* We do NOT rely on the DAT1 interrupt line (not always reliable on jumper
 * wiring).  Instead poll the slave's interrupt *register* over CMD52 and only
 * fetch packets when the new-packet bit is set. */
static void rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t raw = 0, st = 0;
        esp_err_t ret = essl_get_intr(s_essl, &raw, &st, pdMS_TO_TICKS(500));
        if (ret == ESP_OK && raw) {
            essl_clear_intr(s_essl, raw, pdMS_TO_TICKS(500));
            if (raw & ESSL_SDIO_DEF_ESP32.new_packet_intr_mask) {
                drain_packets();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t nn_link_start(void)
{
    if (s_essl) {
        return ESP_OK;          /* already enumerated; idempotent */
    }
    esp_err_t ret = enumerate_slave();
    if (ret != ESP_OK) return ret;

    nn_link__set_connected(true);     /* bus enumerated → master side is up */

    if (xTaskCreate(rx_task, "nn_link_rx", 4096, NULL, 10, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* Kick the handshake so the slave also flips to CONNECTED. */
    NN_LOG_INF("sending HELLO to slave");
    nn_link_send((const uint8_t *)NN_LINK_HELLO, strlen(NN_LINK_HELLO));
    return ESP_OK;
}
