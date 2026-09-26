/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_audio — ES8311 I2S mic capture -> AAC-LC encode on the ESP32-P4.
 *
 * I2S(std, master) <- ES8311 ADC (mic) ; ES8311 controlled over I2C.
 * PCM (16-bit mono) -> esp_audio_codec AAC-LC encoder (ADTS frames) -> callback.
 * A synthetic sine source (CONFIG_NN_AUDIO_SOURCE_SYNTH) replaces the mic for
 * HW-free pipeline testing.
 */
#include "nn_audio/nn_audio.h"
#include "sdkconfig.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_audio);
#include "esp_timer.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_aac_enc.h"

#if CONFIG_NN_AUDIO_SOURCE_ES8311
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#endif


#define SAMPLE_RATE   CONFIG_NN_AUDIO_SAMPLE_RATE
#define AAC_BITRATE   CONFIG_NN_AUDIO_AAC_BITRATE
#define ES8311_ADDR   0x30
#define AUDIO_I2C_PORT 1   /* camera SCCB owns I2C port 0; ES8311 uses port 1 */

static nn_audio_frame_cb_t s_cb;
static void *s_ctx;

static esp_audio_enc_handle_t s_enc;
static int s_in_size, s_out_size;       /* PCM bytes per AAC frame / max AAC out */
static uint8_t *s_pcm, *s_aac;
static uint16_t s_seq;
static uint32_t s_frames; static uint64_t s_bytes;
static volatile bool s_run;

#if CONFIG_NN_AUDIO_SOURCE_ES8311
static esp_codec_dev_handle_t s_dev;

static esp_err_t es8311_bringup(void)
{
    /* I2C master bus for the codec control interface.
     *
     * The ES8311, the camera sensor (IMX708 @0x1A) and the VCM focus motor
     * (DW9714 @0x0C) sit on ONE physical I2C bus (SDA=GPIO7, SCL=GPIO8).  Two
     * I2C master peripherals cannot drive the same pins, so if the camera has
     * already brought up its SCCB bus on port 0 we must SHARE that handle —
     * creating a second master (port 1) on the same pins silently re-muxes the
     * pads and makes every later sensor/motor write NACK (exposure + focus).
     * Fall back to creating our own bus only for audio-only builds. */
    int shared_port = 0;                        /* camera SCCB port */
    i2c_master_bus_handle_t i2c_bus = NULL;
    if (i2c_master_get_bus_handle(shared_port, &i2c_bus) != ESP_OK || !i2c_bus) {
        i2c_master_bus_config_t i2c_cfg = {
            .i2c_port = AUDIO_I2C_PORT,
            .sda_io_num = CONFIG_NN_AUDIO_I2C_SDA_GPIO,
            .scl_io_num = CONFIG_NN_AUDIO_I2C_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&i2c_cfg, &i2c_bus) != ESP_OK) {
            NN_LOG_ERR("i2c bus init failed"); return ESP_FAIL;
        }
        shared_port = AUDIO_I2C_PORT;
        NN_LOG_INF("ES8311 on own I2C bus port %d (camera SCCB absent)", shared_port);
    } else {
        NN_LOG_INF("ES8311 sharing camera SCCB I2C bus on port %d", shared_port);
    }

    /* I2S std channel (ESP is master), RX = mic from ES8311 ASDOUT. */
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, NULL, &rx) != ESP_OK) { NN_LOG_ERR("i2s chan"); return ESP_FAIL; }
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = CONFIG_NN_AUDIO_I2S_MCLK_GPIO,
            .bclk = CONFIG_NN_AUDIO_I2S_BCLK_GPIO,
            .ws   = CONFIG_NN_AUDIO_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = CONFIG_NN_AUDIO_I2S_DIN_GPIO,
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_channel_init_std_mode(rx, &std) != ESP_OK) { NN_LOG_ERR("i2s std"); return ESP_FAIL; }
    i2s_channel_enable(rx);

    /* ES8311 codec (ADC/mic): control over I2C, data over the I2S RX channel. */
    audio_codec_i2c_cfg_t ctrl_cfg = { .port = shared_port, .addr = ES8311_ADDR, .bus_handle = i2c_bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if, .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = -1, .master_mode = true, .use_mclk = true, .digital_mic = false,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) { NN_LOG_ERR("es8311 not found on I2C@0x%02x", ES8311_ADDR); return ESP_FAIL; }

    audio_codec_i2s_cfg_t data_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = NULL };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);

    esp_codec_dev_cfg_t dev_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN,
                                    .codec_if = codec_if, .data_if = data_if };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) { NN_LOG_ERR("codec_dev_new failed"); return ESP_FAIL; }

    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 1,
                                       .channel_mask = 0, .sample_rate = SAMPLE_RATE };
    if (esp_codec_dev_open(s_dev, &fs) != 0) { NN_LOG_ERR("codec_dev_open"); return ESP_FAIL; }
    esp_codec_dev_set_in_gain(s_dev, 30.0f);
    NN_LOG_INF("ES8311 mic up: %d Hz mono 16-bit", SAMPLE_RATE);
    return ESP_OK;
}
#endif /* ES8311 */

static esp_err_t aac_open(void)
{
    esp_aac_enc_register();
    esp_aac_enc_config_t aac = {
        .sample_rate = SAMPLE_RATE, .channel = 1, .bits_per_sample = 16,
        .bitrate = AAC_BITRATE, .adts_used = true,
    };
    esp_audio_enc_config_t cfg = { .type = ESP_AUDIO_TYPE_AAC, .cfg = &aac, .cfg_sz = sizeof(aac) };
    if (esp_audio_enc_open(&cfg, &s_enc) != ESP_AUDIO_ERR_OK) { NN_LOG_ERR("aac open"); return ESP_FAIL; }
    esp_audio_enc_get_frame_size(s_enc, &s_in_size, &s_out_size);
    s_pcm = malloc(s_in_size);
    s_aac = malloc(s_out_size);
    if (!s_pcm || !s_aac) return ESP_ERR_NO_MEM;
    NN_LOG_INF("AAC-LC %d bps: in=%d B/frame out<=%d B", AAC_BITRATE, s_in_size, s_out_size);
    return ESP_OK;
}

static void read_pcm(void)
{
#if CONFIG_NN_AUDIO_SOURCE_ES8311
    esp_codec_dev_read(s_dev, s_pcm, s_in_size);
#else
    /* Synthetic 440 Hz sine, paced to real time. */
    static double ph;
    int n = s_in_size / 2;
    int16_t *p = (int16_t *)s_pcm;
    double step = 2.0 * M_PI * 440.0 / SAMPLE_RATE;
    for (int i = 0; i < n; i++) { p[i] = (int16_t)(8000.0 * sin(ph)); ph += step; if (ph > 2*M_PI) ph -= 2*M_PI; }
    vTaskDelay(pdMS_TO_TICKS(1000 * n / SAMPLE_RATE));
#endif
}

static void cap_task(void *arg)
{
    (void)arg;
    s_run = true;
    while (s_run) {
        read_pcm();
        uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);
        esp_audio_enc_in_frame_t in = { .buffer = s_pcm, .len = s_in_size };
        esp_audio_enc_out_frame_t out = { .buffer = s_aac, .len = s_out_size };
        if (esp_audio_enc_process(s_enc, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0) {
            s_frames++; s_bytes += out.encoded_bytes;
            if (s_cb) s_cb(s_aac, out.encoded_bytes, ts, s_seq++, s_ctx);
        }
    }
    vTaskDelete(NULL);
}

void nn_audio_set_frame_cb(nn_audio_frame_cb_t cb, void *ctx) { s_cb = cb; s_ctx = ctx; }

esp_err_t nn_audio_init(void)
{
#if CONFIG_NN_AUDIO_SOURCE_ES8311
    esp_err_t r = es8311_bringup();
    if (r != ESP_OK) return r;
#endif
    return aac_open();
}

esp_err_t nn_audio_start(void)
{
    if (!s_enc) return ESP_ERR_INVALID_STATE;
    return xTaskCreate(cap_task, "nn_aud_cap", 8192, NULL, 7, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void nn_audio_status(char *out, size_t n)
{
    const char *src =
#if CONFIG_NN_AUDIO_SOURCE_ES8311
        "es8311";
#else
        "synth";
#endif
    snprintf(out, n, "audio: src=%s %dHz aac=%dbps frames=%lu bytes=%llu",
             src, SAMPLE_RATE, AAC_BITRATE, (unsigned long)s_frames, (unsigned long long)s_bytes);
}

static int cmd_aud(int argc, char **argv)
{
    (void)argc; (void)argv;
    char line[128]; nn_audio_status(line, sizeof line); printf("%s\n", line);
    return 0;
}

void nn_audio_cli_register(void)
{
    const esp_console_cmd_t c = { .command = "aud", .help = "Audio capture/encode stats", .func = cmd_aud };
    esp_console_cmd_register(&c);
}
