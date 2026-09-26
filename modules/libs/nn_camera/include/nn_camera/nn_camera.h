/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * nn_camera — ESP32-P4 MIPI-CSI camera capture + hardware H.264 encode.
 *
 * Pipeline (no software color conversion):
 *   OV5647 (RAW8) -> CSI -> ISP (RAW8 -> Espressif YUV420 / OUYY_EVYY)
 *                 -> hardware H.264 encoder -> Annex-B bitstream
 *
 * A capture task pulls each frame, encodes it, and hands the encoded frame to
 * a callback (which the app forwards over the SPI link to the C6, or stores).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* One encoded H.264 frame.  `data`/`len` are owned by nn_camera and valid only
 * for the duration of the callback — copy if you need to keep it. */
typedef void (*nn_camera_frame_cb_t)(const uint8_t *data, size_t len,
                                     bool keyframe, void *ctx);

typedef struct {
    uint32_t frames;        /* frames captured + encoded            */
    uint32_t keyframes;     /* IDR frames                           */
    uint32_t last_size;     /* bytes of the most recent encoded fr. */
    uint64_t total_bytes;   /* cumulative encoded bytes             */
    uint32_t drops;         /* frames dropped (encode/queue errors) */
    int      width, height; /* active capture resolution            */
    uint32_t csi_get;       /* CSI on_get_new_trans callback count  */
    uint32_t csi_done;      /* CSI on_trans_finished callback count */
} nn_camera_stats_t;

/* Bring up the sensor, CSI, ISP and H.264 encoder.  Does not start capture. */
esp_err_t nn_camera_init(void);

/* Register the callback invoked for every encoded frame. */
void nn_camera_set_frame_cb(nn_camera_frame_cb_t cb, void *ctx);

/* Start / stop the capture+encode task. */
esp_err_t nn_camera_start(void);
esp_err_t nn_camera_stop(void);

/* Snapshot the running stats. */
void nn_camera_get_stats(nn_camera_stats_t *out);

/* Detected sensor name (or "?" before init). */
const char *nn_camera_sensor_name(void);

/* Scan the SCCB (I2C) bus and log every address that ACKs — diagnose
 * camera wiring / power / pin config.  Returns ESP_OK if any device found. */
esp_err_t nn_camera_i2c_scan(void);

/* Register the `cam` console command (info/start/stop/stats/snap). */
void nn_camera_cli_register(void);

/* ── Runtime encoder controls (adaptive streaming) ───────────────────────
 * All safe to call while streaming; applied on the next encoded frame. */

/* Target H.264 bitrate in bits/s (V4L2_CID_MPEG_VIDEO_BITRATE). */
esp_err_t nn_camera_set_bitrate(uint32_t bps);

/* GOP / I-frame period in frames (V4L2_CID_MPEG_VIDEO_H264_I_PERIOD). */
esp_err_t nn_camera_set_gop(uint32_t frames);

/* Output frame-rate divisor: 1 = every frame, 2 = half, ... (N-1 of every N
 * captured frames are recycled without encoding).  Cheap fps reduction that
 * needs no encoder support (the encoder is memory-to-memory). */
void nn_camera_set_fps_divisor(uint8_t divisor);

/* Configured capture frame rate (for GOP = fps x 2^N math). */
uint32_t nn_camera_get_base_fps(void);

/* RPi Cam3 autofocus VCM position, 0..1023. */
esp_err_t nn_camera_set_focus(int position);

/* ── ISP runtime tuning (no rebuild) + JPEG snapshot ─────────────────────── */
/* Set one ISP control by name (brightness/contrast/saturation/hue/red_balance/
 * blue_balance/gain/exposure/ae_level). */
esp_err_t nn_camera_isp_set(const char *name, int val);

/* Triage: enable/disable one ISP block (ccm|gamma|bf|sharpen|demosaic|wb|lsc).
 * Disable-only in practice; restore by reboot. */
/* Enable/disable one ISP block by name.  Hardware order:
 *   BLC -> BF -> LSC -> Demosaic -> WBG -> CCM -> Gamma -> RGB2YUV -> SHARP
 * Names: bf lsc demosaic wbg ccm gamma sharpen wb af
 * NOTE the IPA rewrites gamma/sharpen/bf within frames, so those toggles do not
 * hold; ccm/wb/lsc/wbg/demosaic do.  BLC has no control (and is unavailable on
 * pre-v3.0 silicon).  Restore defaults by rebooting. */
esp_err_t nn_camera_isp_block_enable(const char *name, bool enable);

/* WBG per-channel gains, milli (1000 = unity); enables the block as a side
 * effect.  This is the hardware white-balance stage — upstream esp_video does
 * not use it and folds balances into the CCM instead. */
esp_err_t nn_camera_isp_wbg_gain(uint32_t r_milli, uint32_t g_milli, uint32_t b_milli);

/* Re-apply this firmware's boot ISP configuration (tuned CCM, all toggleable
 * blocks enabled, neutral image controls).  NOT a silicon reset: IPA-owned
 * blocks (gamma/sharpen/bf) self-heal each frame regardless, and a disabled
 * block returns enabled but with whatever parameters the last writer left.
 * Reboot for a guaranteed clean slate.  Returns settings re-applied. */
/* ── Interactive block tuning ─────────────────────────────────────────────
 * Each of these TAKES THE IPA HOLD for its block, because the IPA otherwise
 * rewrites bf/sharpen/demosaic every frame and the value never survives long
 * enough to judge.  nn_camera_isp_reset() releases all holds. */
esp_err_t nn_camera_isp_bf(int level, const uint8_t matrix[9]);          /* level 2..20 */
esp_err_t nn_camera_isp_sharpen(int h_thresh, int l_thresh,
                                float h_coeff, float m_coeff);           /* coeffs 0..8  */
esp_err_t nn_camera_isp_demosaic(float gradient_ratio);
esp_err_t nn_camera_isp_hold(uint32_t mask);   /* ESP_VIDEO_IPA_HOLD_* bits, 0 = release */

int nn_camera_isp_reset(void);

/* Push one capture-device frame to a TCP receiver (media/tools/diag_recv.py).
 * Header: [16B desc][u32 w,h,fourcc,stride,slice_height,size][data]. */
esp_err_t nn_camera_diag_send(const char *host, uint16_t port, const char *desc);

/* MIPI-CSI2 link error statistics: ECC, CRC, frame-sync and lane-sync errors
 * straight out of the CSI host controller.  Use this to decide whether an
 * image artifact arrived over the wire or was produced after it. */
/* Poison each capture buffer before the ISP writes it.  Pixels that come back
 * still holding the fill byte were never written by the ISP. */
/* DIAG builds: switch the live capture format ("raw" = SBGGR8, "rgb" = RGB24)
 * without a reboot, so RAW and RGB frames can be interleaved on one boot. */
esp_err_t nn_camera_diag_set_format(const char *name);

/* ISP memory-to-memory replay (RAW diag boot only): snap one RAW frame, tear
 * the pipeline down, push that same frame through the ISP N times via DW-GDMA
 * and send every RGB output (plus the input) to the diag receiver.  The
 * pipeline stays down afterwards — reboot to restore. */
esp_err_t nn_camera_diag_m2m(const char *host, uint16_t port, int iterations);

void      nn_camera_fill_enable(bool on);
bool      nn_camera_fill_enabled(void);

void      nn_camera_diag_csi_dump(void);
void      nn_camera_diag_csi_reset(void);

/* Periodic WebSocket push to the hub (DIAG builds).  It shares the single
 * capture loop with nn_camera_diag_send(), and one full-frame push blocks that
 * loop for 10-20 s inside an uninterruptible socket write, so switch it off
 * while driving on-demand captures. */
void      nn_camera_diag_ws_enable(bool on);
bool      nn_camera_diag_ws_enabled(void);

/* Direct ISP white balance: CCM diagonal gains (green fixed at 1.0). */
esp_err_t nn_camera_set_wb(float r_gain, float b_gain);
/* Set the full 3x3 ISP colour-correction matrix directly (row-major m[9]).
 * Retries across frame boundaries (shadow-register latch). Used for host-
 * computed reference calibration; overrides the CCM_BASE x WB-gain path. */
esp_err_t nn_camera_set_ccm(const float m[9]);
void nn_camera_get_wb(float *r, float *b);
esp_err_t nn_camera_isp_get(const char *name, int *val);
uint8_t nn_camera_get_fps_divisor(void);

/* Force the next encoded frame to be an IDR (fast viewer join). */
void nn_camera_force_idr(void);
/* Write the current ISP settings as "name=value\n" text; returns length. */
size_t nn_camera_isp_dump(char *buf, size_t cap);
/* Capture one H.264 keyframe (SPS+PPS+IDR) into an internal PSRAM buffer
 * (valid until the next capture); the host decodes it to a still image. */
esp_err_t nn_camera_capture_idr(const uint8_t **out, size_t *len, int timeout_ms);

#ifdef __cplusplus
}
#endif
