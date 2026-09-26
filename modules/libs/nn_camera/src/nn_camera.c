/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_camera — ESP32-P4 camera -> hardware H.264 via the esp_video (V4L2) stack.
 *
 * esp_video_init() owns the whole MIPI-CSI / ISP / sensor bring-up (the tested
 * path).  We then use standard V4L2:
 *   /dev/video0   capture (camera, YUV420)
 *   /dev/video11  H.264 hardware encoder (M2M: YUV420 in -> H.264 out)
 *
 * Per frame: DQBUF the camera buffer, feed it to the encoder OUTPUT queue,
 * DQBUF the encoder CAPTURE queue (the H.264 bitstream), hand it to the frame
 * callback (which forwards over the SPI link), then recycle the buffers.
 */
#include "nn_camera/nn_camera.h"
#include "nn_camera_internal.h"
#include "sdkconfig.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_camera);
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "driver/ppa.h"
#include "esp_video_init.h"
#include "esp_cam_motor.h"
#if CONFIG_ESP_VIDEO_ENABLE_CAMERA_MOTOR_CONTROLLER
/* internal esp_video accessor for the CSI-attached AF motor */
extern esp_cam_motor_device_t *esp_video_get_csi_video_device_motor(void);
#endif
#include "nvs.h"
#include "esp_cache.h"
#include "esp_video_device.h"
#include "esp_video_isp_ioctl.h"
#include "linux/videodev2.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#if CONFIG_NN_CAMERA_DIAG_ISP
#include <time.h>
/* WS transport lives in nn_camera_diag.c (its lwip headers clash with V4L2). */
#include "nn_camera_diag.h"   /* diag/ surface: WS sink + TCP frame tap */
void  nn_cam_diag_close(void *h);
#endif


#define CAM_DEV    ESP_VIDEO_MIPI_CSI_DEVICE_NAME    /* /dev/video0  */
#define ENC_DEV    ESP_VIDEO_H264_DEVICE_NAME        /* /dev/video11 */
#define WIDTH      CONFIG_NN_CAMERA_HRES             /* H.264 (encoder) width  */
#define HEIGHT     CONFIG_NN_CAMERA_VRES             /* H.264 (encoder) height */
/* Sensor/ISP capture geometry.  The ISP has no scaler and the P4 H.264 encoder
 * tops out at 1920 wide, so a >1080p sensor mode (e.g. IMX708 2304x1296) is
 * HW-downscaled to WIDTHxHEIGHT by the PPA — a scale, not a crop, so the full
 * field of view is preserved.  Defaults (Kconfig) to WIDTHxHEIGHT => no scale. */
#ifndef CONFIG_NN_CAMERA_CAP_HRES
#  define CONFIG_NN_CAMERA_CAP_HRES CONFIG_NN_CAMERA_HRES
#endif
#ifndef CONFIG_NN_CAMERA_CAP_VRES
#  define CONFIG_NN_CAMERA_CAP_VRES CONFIG_NN_CAMERA_VRES
#endif
#define CAP_W      CONFIG_NN_CAMERA_CAP_HRES
#define CAP_H      CONFIG_NN_CAMERA_CAP_VRES
#define SCALE_EN   (CAP_W != WIDTH || CAP_H != HEIGHT)
/* Scaling needs two extra full-frame PPA buffers in PSRAM; at the large capture
 * geometry (e.g. 2304x1296) the CSI controller also allocates a big backup
 * buffer at STREAMON, so trim the capture ring to 2 to leave it room (STREAMON
 * = ENOMEM otherwise).  Non-scaling builds keep the deeper 3-deep ring. */
#define CAP_BUFS   (SCALE_EN ? 2 : 3)
#if CONFIG_NN_CAMERA_DIAG_ISP
/* DIAG_RAW_BAYER: 1 = capture RAW8 Bayer (pre-demosaic — tests whether the ISP
 * demosaic line buffer is what wraps at 2048); 0 = RGB888 (post-ISP). */
#define DIAG_RAW_BAYER 0
#if DIAG_RAW_BAYER
#define CAP_FMT      V4L2_PIX_FMT_SBGGR8
#define DIAG_FMT_STR "BGGR8"
#define DIAG_BPP     1
#else
#define CAP_FMT      V4L2_PIX_FMT_RGB24
#define DIAG_FMT_STR "RGB888"
#define DIAG_BPP     3
#endif
#else
#define CAP_FMT    V4L2_PIX_FMT_YUV420               /* camera + encoder input */
#endif
/* DIAGNOSTIC: 1 = single PPA center-crop (no scale) to isolate the misalignment
 * (if the output is clean, the fault is in the x0.875 scale pass). 0 = normal
 * 2-pass scale+crop. */
#define DIAG_CROP_ONLY 1

/* ── Per-sensor tuning ────────────────────────────────────────────────────
 * Selected at compile time from the esp_cam_sensor driver choice, so a new
 * sensor/lens is one more #elif (name + its calibrated CCM), not a source edit
 * in the WB path.  The base CCM models sensor colour crosstalk; per-channel WB
 * gains (NN_CAMERA_WB_*_MILLI) scale its red/blue columns at runtime. */
#if CONFIG_CAMERA_IMX708
#  define NN_CAM_SENSOR_NAME "IMX708"
/* RPi libcamera's calibrated IMX708 CCM @4640K (indoor/mixed) — much stronger
 * colour separation than the fork's generic matrix. */
static const float CCM_BASE[3][3] = {
    {  1.530f, -0.352f, -0.178f },
    { -0.283f,  1.671f, -0.388f },
    {  0.017f, -0.572f,  1.555f },
};
#elif CONFIG_CAMERA_OV5647
#  define NN_CAM_SENSOR_NAME "OV5647"
static const float CCM_BASE[3][3] = {   /* identity until an OV5647 CCM is tuned */
    { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
};
#else
#  define NN_CAM_SENSOR_NAME "generic"
static const float CCM_BASE[3][3] = {   /* identity: no crosstalk model */
    { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
};
#endif

static int       s_cap_fd = -1;
static int       s_m2m_fd = -1;
static uint8_t  *s_cap_buf[CAP_BUFS];
static uint8_t  *s_enc_buf;                          /* mmap'd H.264 output */
#if SCALE_EN
/* The PPA scaler has only 4-bit fractional precision (1/16 steps, forced even
 * for YUV420), so an arbitrary ratio like 2304->1920 (0.8333) is NOT
 * representable and would leave an unwritten (green) margin.  Instead we use the
 * largest exactly-representable downscale that still covers WIDTH/HEIGHT, then a
 * second PPA pass center-crops to the exact encoder size.  Net FOV loss is just
 * the crop of the (already full-frame) scaled image — far less than a raw crop.
 *   0.875 = 14/16 (even) : 2304x1296 -> 2016x1134 (full frame) -> crop 1920x1088 */
#define MID_W   (CAP_W * 7 / 8)                      /* x0.875, exactly representable */
#define MID_H   (CAP_H * 7 / 8)
#define CROP_X  (((MID_W - WIDTH)  / 2) & ~1)        /* even offsets req. for YUV420 */
#define CROP_Y  (((MID_H - HEIGHT) / 2) & ~1)
static ppa_client_handle_t s_ppa;                    /* PPA SRM scaler+cropper  */
static uint8_t  *s_mid_buf;                          /* pass-1 out: MID_WxMID_H  */
static size_t    s_mid_sz;
static uint8_t  *s_scaled_buf;                       /* pass-2 out: WIDTHxHEIGHT */
static size_t    s_scaled_sz;
static size_t    s_scaled_len;                       /* length fed to encoder    */
#endif
static nn_camera_frame_cb_t s_cb;
static void     *s_cb_ctx;
static volatile bool s_running;
static TaskHandle_t  s_task;
static nn_camera_stats_t s_stats;
static volatile uint8_t s_fps_divisor = 1;   /* 1 = encode every captured frame */

/* ── On-demand H.264 IDR snapshot for ISP tuning: capture one keyframe (tens
 *    of KB — fast uplink vs a ~2MB raw frame) and let the host decode it. ── */
#define SNAP_MAX       (256 * 1024)          /* one H.264 keyframe (SPS+PPS+IDR) */
static uint8_t         *s_snap_out;          /* PSRAM copy of the IDR */
static volatile bool    s_diag_want;         /* diag tap: grab next frame */
/* The periodic WebSocket sink competes with the on-demand tap for the single
 * capture loop, and a 7.46 MB push blocks for 10-20 s inside ONE socket write
 * that cannot be interrupted.  Let the operator switch it off (`isp ws 0`) when
 * driving captures to a dev host, which is the only way to make `isp send`
 * responsive rather than hostage to the hub link. */
static volatile bool    s_diag_ws_on = true;
static uint8_t * volatile s_diag_buf;        /* frame handed to the tap */
/* Poison the capture buffer before handing it to the ISP.  Any pixel that comes
 * back still holding the signature was NEVER WRITTEN by the ISP — which
 * distinguishes "the ISP produced bad pixels" from "the ISP skipped this region
 * and we are looking at stale memory".  0x5A is chosen because it is not a
 * plausible luma/chroma value for this scene and forms an obvious 2-px pattern
 * in either RGB888 or the OUYY layout. */
#define FILL_BYTE 0x5A
static volatile bool    s_fill;
#if CONFIG_NN_CAMERA_DIAG_ISP
/* Runtime-switchable capture format (DIAG only).  The point is to interleave
 * RAW Bayer and RGB888 captures SECONDS apart on one boot: the earlier
 * RAW-clean-vs-RGB-banded comparison had its two arms separated by a reflash,
 * and band incidence is known to vary with an uncontrolled boot-state variable,
 * so same-boot interleaving is the only clean version of that experiment. */
static uint32_t s_cap_fmt  = CAP_FMT;
static uint32_t s_diag_bpp = DIAG_BPP;
static void diag_load_format(void);
static volatile bool s_diag_task_gone;
#else
#define s_cap_fmt  CAP_FMT
#define s_diag_bpp DIAG_BPP
#endif

/* Fill a capture buffer with FILL_BYTE and push it out of the CPU cache so the
 * ISP's DMA sees the poison rather than stale cache lines. */
static void poison_buffer(uint8_t *buf, size_t len)
{
    if (!buf || !len) return;
    memset(buf, FILL_BYTE, len);
    esp_cache_msync(buf, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static uint8_t         *s_diag_copy;         /* optional private snapshot */
static size_t           s_diag_copy_sz;
static volatile uint32_t s_diag_len;

/* Internals shared with diag/nn_camera_diag_tap.c — the tap needs the capture
 * fd (for G_FMT) and the borrowed-frame slot, but everything else here stays
 * private.  Declared in nn_camera_internal.h. */
int      nn_camera__cap_fd(void)        { return s_cap_fd; }
void     nn_camera__diag_arm(void)      { s_diag_len = 0; s_diag_want = true; }
void     nn_camera_diag_ws_enable(bool on) { s_diag_ws_on = on; }
void     nn_camera_fill_enable(bool on)    { s_fill = on; }
bool     nn_camera_fill_enabled(void)      { return s_fill; }
bool     nn_camera_diag_ws_enabled(void)   { return s_diag_ws_on; }
void     nn_camera__diag_disarm(void)   { s_diag_want = false; }
uint32_t nn_camera__diag_len(void)      { return s_diag_len; }
const uint8_t *nn_camera__diag_buf(void){ return s_diag_buf; }
static volatile size_t  s_snap_len;
static volatile bool    s_want_snap;
static volatile bool    s_snap_restart;   /* restart encoder → first frame is an IDR */
static volatile bool    s_snap_lock;      /* hold GOP=1 during a snapshot */
static SemaphoreHandle_t s_snap_sem;
static SemaphoreHandle_t s_m2m_mtx;   /* encoder restart vs live ctrl races */

const char *nn_camera_sensor_name(void) { return NN_CAM_SENSOR_NAME " (esp_video/V4L2)"; }
void nn_camera_set_frame_cb(nn_camera_frame_cb_t cb, void *ctx) { s_cb = cb; s_cb_ctx = ctx; }
void nn_camera_get_stats(nn_camera_stats_t *o) { if (o) *o = s_stats; }
uint32_t nn_camera_get_base_fps(void) { return CONFIG_NN_CAMERA_H264_FPS; }
void nn_camera_set_fps_divisor(uint8_t d) { s_fps_divisor = d ? d : 1; }

/* RPi Cam3 VCM focus, 0..1023 (0 = one extreme, 1023 = the other).  Routed to
 * the DW9714 motor via the sensor video device's V4L2_CID_FOCUS_ABSOLUTE. */
esp_err_t nn_camera_set_focus(int position)
{
    if (s_cap_fd < 0) return ESP_ERR_INVALID_STATE;
    if (position < 0) position = 0;
    if (position > CONFIG_NN_CAMERA_FOCUS_MAX) position = CONFIG_NN_CAMERA_FOCUS_MAX;
#if CONFIG_ESP_VIDEO_ENABLE_CAMERA_MOTOR_CONTROLLER
    esp_cam_motor_device_t *m = esp_video_get_csi_video_device_motor();
    if (!m) { NN_LOG_WRN("no VCM motor detected"); return ESP_ERR_INVALID_STATE; }
    int v = position;
    esp_err_t r = esp_cam_motor_set_para_value(m, ESP_CAM_MOTOR_POSITION_CODE, &v, sizeof(v));
    if (r != ESP_OK) { NN_LOG_WRN("focus %d: %s", position, esp_err_to_name(r)); return r; }
    NN_LOG_INF("focus -> %d", position);
    return ESP_OK;
#else
    (void)position; return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* esp_video hardware config: MIPI-CSI sensor (esp_cam_sensor-selected) over SCCB (I2C). */
static const esp_video_init_csi_config_t s_csi_cfg[] = {{
    .sccb_config = {
        .init_sccb = true,
        .i2c_config = {
            .port    = 0,
            .scl_pin = CONFIG_NN_CAMERA_SCCB_SCL_IO,
            .sda_pin = CONFIG_NN_CAMERA_SCCB_SDA_IO,
        },
        .freq = 100000,
    },
    .reset_pin = -1,
    .pwdn_pin  = -1,
}};
#if CONFIG_ESP_VIDEO_ENABLE_CAMERA_MOTOR_CONTROLLER
/* RPi Cam Module 3 autofocus VCM (DW9714-class @ I2C 0x0C) — shares the sensor
 * SCCB bus (same port/pins → esp_video reuses the existing I2C master). */
static const esp_video_init_cam_motor_config_t s_motor_cfg = {
    .sccb_config = {
        .init_sccb = true,
        .i2c_config = { .port = 0,
                        .scl_pin = CONFIG_NN_CAMERA_SCCB_SCL_IO,
                        .sda_pin = CONFIG_NN_CAMERA_SCCB_SDA_IO },
        .freq = 100000,
    },
    .reset_pin = -1, .pwdn_pin = -1, .signal_pin = -1,
};
#endif
static const esp_video_init_config_t s_video_cfg = {
    .csi = s_csi_cfg,
#if CONFIG_ESP_VIDEO_ENABLE_CAMERA_MOTOR_CONTROLLER
    .cam_motor = &s_motor_cfg,
#endif
};

esp_err_t nn_camera_i2c_scan(void)
{
    NN_LOG_INF("SCCB is managed by esp_video; sensor probed during init");
    return ESP_OK;
}

static int set_ctrl(int fd, uint32_t id, int32_t val)
{
    struct v4l2_ext_control c = { .id = id, .value = val };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_CODEC, .count = 1, .controls = &c };
    /* Live encoder ctrls race the capture task's encoder restart (the fork
     * h264 device dereferences enc_handle, which restart deletes/recreates):
     * serialize every m2m touch.  A zero-byte-forever encoder was the price. */
    if (fd == s_m2m_fd && s_m2m_mtx) {
        if (xSemaphoreTake(s_m2m_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return -1;
        int r = ioctl(fd, VIDIOC_S_EXT_CTRLS, &cs);
        xSemaphoreGive(s_m2m_mtx);
        return r;
    }
    return ioctl(fd, VIDIOC_S_EXT_CTRLS, &cs);
}

esp_err_t nn_camera_set_bitrate(uint32_t bps)
{
    if (s_m2m_fd < 0) return ESP_ERR_INVALID_STATE;
    return set_ctrl(s_m2m_fd, V4L2_CID_MPEG_VIDEO_BITRATE, (int32_t)bps) == 0
               ? ESP_OK : ESP_FAIL;
}

esp_err_t nn_camera_set_gop(uint32_t frames)
{
    if (s_m2m_fd < 0) return ESP_ERR_INVALID_STATE;
    /* While a snapshot is in progress, ignore adaptive-controller GOP changes so
     * the forced GOP=1 (all-IDR) holds long enough to grab a real keyframe. */
    if (s_snap_lock && frames != 1) return ESP_OK;
    if (frames > 240) frames = 240;   /* esp_h264 gop is uint8; stored value is
                                       * also reused as encoder cfg on restart */
    return set_ctrl(s_m2m_fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, (int32_t)frames) == 0
               ? ESP_OK : ESP_FAIL;
}

/* ── ISP runtime controls (tunable without rebuild) ──────────────────────
 * Simple int controls on the camera/ISP video device.  Set/read live via the
 * `isp` console command; the current values go into each JPEG snapshot's .txt. */
static const struct { const char *name; uint32_t cid; } s_isp_ctrls[] = {
    { "brightness",   V4L2_CID_BRIGHTNESS },
    { "contrast",     V4L2_CID_CONTRAST },
    { "saturation",   V4L2_CID_SATURATION },
    { "hue",          V4L2_CID_HUE },
    { "red_balance",  V4L2_CID_RED_BALANCE },
    { "blue_balance", V4L2_CID_BLUE_BALANCE },
    { "gain",         V4L2_CID_GAIN },
    { "exposure",     V4L2_CID_EXPOSURE },
};

static int isp_get_ctrl(uint32_t cid, int32_t *val)
{
    struct v4l2_ext_control c = { .id = cid };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_USER, .count = 1, .controls = &c };
    if (ioctl(s_cap_fd, VIDIOC_G_EXT_CTRLS, &cs) != 0) return -1;
    *val = c.value; return 0;
}

/* Direct white balance: write the ISP CCM (via the ISP video device) as a
 * diagonal gain matrix.  This is the only WB lever that provably reaches the
 * hardware: the V4L2 red/blue_balance ctrls on the capture device land on
 * sensor registers the AGC fights, and the IPA (when enabled) rewrites the
 * CCM every frame.  Requires the IPA awb/acc blocks removed from the JSON. */
/* Force the next encoded frame to be an IDR (fresh joiner fast-start). */
void nn_camera_force_idr(void)
{
    s_snap_restart = true;
}

static float s_wb_r = 1.0f, s_wb_b = 1.0f;   /* last-applied WB gains */
void nn_camera_get_wb(float *r, float *b) { if (r) *r = s_wb_r; if (b) *b = s_wb_b; }
uint8_t nn_camera_get_fps_divisor(void) { return s_fps_divisor; }

/* Triage tool: enable/disable one ISP pipeline block via its V4L2 ext control.
 * Used to bisect which block manufactures the 16-px left-edge chroma band
 * (RAW measured clean, ISP RGB out banded).  DISABLE ONLY in normal use — the
 * enable=true path writes a zeroed config, so restore blocks by rebooting.
 * Each disable has an unmistakable global look (sharpen->soft, gamma->dark,
 * ccm->desaturated, bf->noisy, demosaic->broken), which doubles as proof the
 * toggle actually took effect (the IPA may rewrite some blocks). */
/* Set the WBG block's per-channel gains (milli; 1000 = unity) and enable it. */
/* Restore the ISP to this firmware's boot configuration.
 *
 * HONEST SCOPE — this is "re-apply our defaults", not "reload silicon reset
 * values", and the two differ:
 *   - blocks WE own (CCM from the Kconfig WB gains, WBG, simple V4L2 controls)
 *     are rewritten here and take effect immediately;
 *   - blocks the IPA owns (gamma, sharpen, bf) are rewritten by the IPA every
 *     frame anyway, so they self-heal within ~a frame whatever you did to them;
 *   - a block you DISABLED with `isp blk X 0` is re-enabled here, but its
 *     internal parameters are whatever the last writer left — for a guaranteed
 *     clean slate, reboot.
 * Returns the number of settings successfully re-applied. */
/* ── Interactive block tuning ─────────────────────────────────────────────
 * Each setter TAKES THE IPA HOLD for its block first, otherwise the IPA
 * overwrites the value within ~a frame and the experiment measures nothing.
 * `isp reset` (or a reboot) releases every hold. */
static esp_err_t isp_write_ctrl(uint32_t cid, void *cfg, size_t sz)
{
    static int s_fd = -1;
    if (s_fd < 0) {
        s_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
        if (s_fd < 0) return ESP_FAIL;
    }
    struct v4l2_ext_control c = { .id = cid, .size = (uint32_t)sz, .p_u8 = cfg };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_USER,
                                    .count = 1, .controls = &c };
    return ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &cs) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t nn_camera_isp_bf(int level, const uint8_t matrix[9])
{
    if (level < 2 || level > 20) return ESP_ERR_INVALID_ARG;   /* HW range */
    esp_video_isp_bf_t bf = { .enable = true, .level = (uint8_t)level };
    static const uint8_t dflt[9] = { 1, 2, 1, 2, 4, 2, 1, 2, 1 };  /* gaussian */
    const uint8_t *m = matrix ? matrix : dflt;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) bf.matrix[i][j] = m[i * 3 + j];

    esp_video_isp_ipa_hold_set(esp_video_isp_ipa_hold_get() | ESP_VIDEO_IPA_HOLD_BF);
    esp_err_t r = isp_write_ctrl(V4L2_CID_USER_ESP_ISP_BF, &bf, sizeof bf);
    NN_LOG_INF("bf level=%d (IPA hold on) -> %s", level, esp_err_to_name(r));
    return r;
}

esp_err_t nn_camera_isp_sharpen(int h_thresh, int l_thresh, float h_coeff, float m_coeff)
{
    if (h_thresh < 0 || h_thresh > 255 || l_thresh < 0 || l_thresh > 255)
        return ESP_ERR_INVALID_ARG;
    esp_video_isp_sharpen_t sh = {
        .enable = true, .h_thresh = (uint8_t)h_thresh, .l_thresh = (uint8_t)l_thresh,
        .h_coeff = h_coeff, .m_coeff = m_coeff,
    };
    static const uint8_t dflt[9] = { 1, 2, 1, 2, 2, 2, 1, 2, 1 };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) sh.matrix[i][j] = dflt[i * 3 + j];

    esp_video_isp_ipa_hold_set(esp_video_isp_ipa_hold_get() | ESP_VIDEO_IPA_HOLD_SHARPEN);
    esp_err_t r = isp_write_ctrl(V4L2_CID_USER_ESP_ISP_SHARPEN, &sh, sizeof sh);
    NN_LOG_INF("sharpen h=%d l=%d hc=%.2f mc=%.2f (IPA hold on) -> %s",
               h_thresh, l_thresh, h_coeff, m_coeff, esp_err_to_name(r));
    return r;
}

esp_err_t nn_camera_isp_demosaic(float gradient_ratio)
{
    esp_video_isp_demosaic_t dm = { .enable = true, .gradient_ratio = gradient_ratio };

    esp_video_isp_ipa_hold_set(esp_video_isp_ipa_hold_get() | ESP_VIDEO_IPA_HOLD_DEMOSAIC);
    esp_err_t r = isp_write_ctrl(V4L2_CID_USER_ESP_ISP_DEMOSAIC, &dm, sizeof dm);
    NN_LOG_INF("demosaic gradient_ratio=%.3f (IPA hold on) -> %s",
               gradient_ratio, esp_err_to_name(r));
    return r;
}

esp_err_t nn_camera_isp_hold(uint32_t mask)
{
    esp_video_isp_ipa_hold_set(mask);
    NN_LOG_INF("IPA hold mask = 0x%02X (bf=%d dm=%d sh=%d gamma=%d ccm=%d)",
               (unsigned)mask,
               !!(mask & ESP_VIDEO_IPA_HOLD_BF), !!(mask & ESP_VIDEO_IPA_HOLD_DEMOSAIC),
               !!(mask & ESP_VIDEO_IPA_HOLD_SHARPEN), !!(mask & ESP_VIDEO_IPA_HOLD_GAMMA),
               !!(mask & ESP_VIDEO_IPA_HOLD_CCM));
    return ESP_OK;
}

int nn_camera_isp_reset(void)
{
    int ok = 0;

    /* release every IPA hold first, so the IPA resumes owning its blocks */
    esp_video_isp_ipa_hold_set(0);

    /* colour: the tuned CCM (CCM_BASE x Kconfig WB gains) */
    if (nn_camera_set_wb((float)CONFIG_NN_CAMERA_WB_R_MILLI / 1000.0f,
                         (float)CONFIG_NN_CAMERA_WB_B_MILLI / 1000.0f) == ESP_OK) ok++;

    /* re-enable every block we can toggle (IPA-owned ones self-heal regardless) */
    static const char *blocks[] = { "bf", "demosaic", "ccm", "gamma", "sharpen" };
    for (size_t i = 0; i < sizeof blocks / sizeof blocks[0]; i++) {
        if (nn_camera_isp_block_enable(blocks[i], true) == ESP_OK) ok++;
    }

    /* neutral image controls */
    static const struct { const char *n; int v; } ctrls[] = {
        { "brightness", 0 }, { "contrast", 0 }, { "saturation", 0 }, { "hue", 0 },
    };
    for (size_t i = 0; i < sizeof ctrls / sizeof ctrls[0]; i++) {
        if (nn_camera_isp_set(ctrls[i].n, ctrls[i].v) == ESP_OK) ok++;
    }

    NN_LOG_INF("ISP reset: %d setting(s) re-applied (wb r=%.3f b=%.3f); "
               "reboot for a guaranteed clean slate", ok,
               (float)CONFIG_NN_CAMERA_WB_R_MILLI / 1000.0f,
               (float)CONFIG_NN_CAMERA_WB_B_MILLI / 1000.0f);
    return ok;
}

esp_err_t nn_camera_isp_wbg_gain(uint32_t r_milli, uint32_t g_milli, uint32_t b_milli)
{
    static int s_fd = -1;
    if (s_fd < 0) {
        s_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
        if (s_fd < 0) return ESP_FAIL;
    }
    esp_video_isp_wbg_t wbg = { .enable = true, .red_gain = r_milli,
                                .green_gain = g_milli, .blue_gain = b_milli };
    struct v4l2_ext_control c = { .id = V4L2_CID_USER_ESP_ISP_WBG,
                                  .size = sizeof wbg, .p_u8 = (void *)&wbg };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_USER,
                                    .count = 1, .controls = &c };
    if (ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &cs) != 0) {
        NN_LOG_WRN("wbg gain r=%u g=%u b=%u: errno %d",
                   (unsigned)r_milli, (unsigned)g_milli, (unsigned)b_milli, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t nn_camera_isp_block_enable(const char *name, bool enable)
{
    /* Every ISP block esp_video exposes an enable for.  Hardware order is
     *   BLC -> BF -> LSC -> Demosaic -> WBG -> CCM -> Gamma -> RGB2YUV -> SHARP
     * (BLC has no control and is unavailable on pre-v3.0 silicon anyway).
     * CAVEAT measured 2026-08-03: the IPA rewrites gamma/sharpen/bf within
     * frames, so those toggles do not hold — ccm/wb/lsc/wbg/demosaic do. */
    static const struct { const char *n; uint32_t cid; size_t sz; } tbl[] = {
        { "bf",       V4L2_CID_USER_ESP_ISP_BF,       sizeof(esp_video_isp_bf_t) },
        { "lsc",      V4L2_CID_USER_ESP_ISP_LSC,      sizeof(esp_video_isp_lsc_t) },
        { "demosaic", V4L2_CID_USER_ESP_ISP_DEMOSAIC, sizeof(esp_video_isp_demosaic_t) },
        { "wbg",      V4L2_CID_USER_ESP_ISP_WBG,      sizeof(esp_video_isp_wbg_t) },
        { "ccm",      V4L2_CID_USER_ESP_ISP_CCM,      sizeof(esp_video_isp_ccm_t) },
        { "gamma",    V4L2_CID_USER_ESP_ISP_GAMMA,    sizeof(esp_video_isp_gamma_t) },
        { "sharpen",  V4L2_CID_USER_ESP_ISP_SHARPEN,  sizeof(esp_video_isp_sharpen_t) },
        { "wb",       V4L2_CID_USER_ESP_ISP_WB,       sizeof(esp_video_isp_wb_t) },
        { "af",       V4L2_CID_USER_ESP_ISP_AF,       sizeof(esp_video_isp_af_t) },
    };
    static int s_fd = -1;
    if (s_fd < 0) {
        s_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
        if (s_fd < 0) return ESP_FAIL;
    }
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
        if (strcmp(name, tbl[i].n) != 0) continue;
        /* first bool of every esp_video_isp_*_t is `enable` */
        static uint8_t cfg[192];
        if (tbl[i].sz > sizeof cfg) return ESP_ERR_NO_MEM;
        memset(cfg, 0, sizeof cfg);
        cfg[0] = enable ? 1 : 0;
        if (strcmp(name, "wbg") == 0) {
            /* WBG carries gains, not just an enable: default to unity so
             * "on" means "identity", i.e. a no-op the caller can then tune. */
            esp_video_isp_wbg_t *g = (esp_video_isp_wbg_t *)cfg;
            g->red_gain = g->green_gain = g->blue_gain = 1000;
        }
        struct v4l2_ext_control c = { .id = tbl[i].cid, .size = (uint32_t)tbl[i].sz,
                                      .p_u8 = cfg };
        struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_USER,
                                        .count = 1, .controls = &c };
        if (ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &cs) != 0) {
            NN_LOG_WRN("isp blk %s: errno %d", name, errno);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nn_camera_set_wb(float r_gain, float b_gain)
{
    static int s_isp_fd = -1;
    if (s_isp_fd < 0) {
        s_isp_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
        if (s_isp_fd < 0) {
            NN_LOG_ERR("open %s: errno %d", ESP_VIDEO_ISP1_DEVICE_NAME, errno);
            return ESP_FAIL;
        }
    }
    /* Full color matrix = the per-sensor base CCM (crosstalk compensation, see
     * CCM_BASE above) x per-channel WB gains.  A pure diagonal (WB only) renders
     * grey-ish/desaturated because the negative off-diagonal terms are what give
     * the colours separation. */
    esp_video_isp_ccm_t ccm = { .enable = true };
    for (int i = 0; i < 3; i++) {
        ccm.matrix[i][0] = CCM_BASE[i][0] * r_gain;   /* WB applied pre-mix */
        ccm.matrix[i][1] = CCM_BASE[i][1];
        ccm.matrix[i][2] = CCM_BASE[i][2] * b_gain;
    }
    /* Normalise so a neutral gray keeps its luma.  Without this, correcting a
     * green-heavy sensor (gains > 1 on R and B) inflates overall brightness and
     * the picture clips to white. */
    float L = 0.299f * (ccm.matrix[0][0] + ccm.matrix[0][1] + ccm.matrix[0][2])
            + 0.587f * (ccm.matrix[1][0] + ccm.matrix[1][1] + ccm.matrix[1][2])
            + 0.114f * (ccm.matrix[2][0] + ccm.matrix[2][1] + ccm.matrix[2][2]);
    if (L > 0.1f)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                ccm.matrix[i][j] /= L;
    struct v4l2_ext_control c = { .id = V4L2_CID_USER_ESP_ISP_CCM,
                                  .size = sizeof(ccm), .p_u8 = (void *)&ccm };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_USER,
                                    .count = 1, .controls = &c };
    if (ioctl(s_isp_fd, VIDIOC_S_EXT_CTRLS, &cs) != 0) {
        NN_LOG_WRN("wb ccm set r=%.2f b=%.2f: errno %d", r_gain, b_gain, errno);
        return ESP_FAIL;
    }
    s_wb_r = r_gain; s_wb_b = b_gain;
    NN_LOG_INF("wb ccm: r=%.2f g=1.00 b=%.2f", r_gain, b_gain);
    return ESP_OK;
}

esp_err_t nn_camera_set_ccm(const float m[9])
{
    static int s_ccm_fd = -1;
    if (s_ccm_fd < 0) {
        s_ccm_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
        if (s_ccm_fd < 0) {
            NN_LOG_ERR("open %s: errno %d", ESP_VIDEO_ISP1_DEVICE_NAME, errno);
            return ESP_FAIL;
        }
    }
    esp_video_isp_ccm_t ccm = { .enable = true };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            ccm.matrix[i][j] = m[i * 3 + j];
    struct v4l2_ext_control c = { .id = V4L2_CID_USER_ESP_ISP_CCM,
                                  .size = sizeof(ccm), .p_u8 = (void *)&ccm };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_USER,
                                    .count = 1, .controls = &c };
    /* Shadow register latches at a frame boundary; retry across frames. */
    for (int i = 0; i < 25; i++) {
        if (ioctl(s_ccm_fd, VIDIOC_S_EXT_CTRLS, &cs) == 0) {
            NN_LOG_INF("ccm set [%.3f %.3f %.3f; %.3f %.3f %.3f; %.3f %.3f %.3f]",
                       m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    NN_LOG_WRN("ccm set: errno %d", errno);
    return ESP_FAIL;
}

/* The ISP CCM shadow register latches only at a frame boundary, so setting WB
 * right after STREAMON races the ISP's own init CCM update and returns EBUSY
 * ("failed to update ccm shadow register", errno 16 — only bites tall frames;
 * 720p slipped through, 1296-tall doesn't).  Let a frame pass, then retry until
 * it latches (else the picture keeps the raw sensor green/magenta cast). */
static void apply_wb_retry(void)
{
    float r = (float)CONFIG_NN_CAMERA_WB_R_MILLI / 1000.0f;
    float b = (float)CONFIG_NN_CAMERA_WB_B_MILLI / 1000.0f;
    for (int i = 0; i < 25; i++) {
        vTaskDelay(pdMS_TO_TICKS(60));   /* wait for a frame boundary first */
        if (nn_camera_set_wb(r, b) == ESP_OK) {
            NN_LOG_INF("wb ccm latched after %d frame(s)", i + 1);
            return;
        }
    }
    NN_LOG_WRN("wb ccm never latched — picture will keep the sensor colour cast");
}

/* Public read of a named ISP control (see s_isp_ctrls). */
esp_err_t nn_camera_isp_get(const char *name, int *val)
{
    if (s_cap_fd < 0) return ESP_ERR_INVALID_STATE;
    for (int i = 0; i < (int)(sizeof s_isp_ctrls / sizeof s_isp_ctrls[0]); i++) {
        if (strcmp(name, s_isp_ctrls[i].name) == 0) {
            int32_t v;
            if (isp_get_ctrl(s_isp_ctrls[i].cid, &v) != 0) return ESP_FAIL;
            *val = (int)v;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nn_camera_isp_set(const char *name, int val)
{
    if (s_cap_fd < 0) return ESP_ERR_INVALID_STATE;
    for (int i = 0; i < (int)(sizeof s_isp_ctrls / sizeof s_isp_ctrls[0]); i++) {
        if (strcmp(name, s_isp_ctrls[i].name) == 0) {
            struct v4l2_ext_control c = { .id = s_isp_ctrls[i].cid, .value = val };
            struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_USER, .count = 1, .controls = &c };
            if (ioctl(s_cap_fd, VIDIOC_S_EXT_CTRLS, &cs) != 0) {
                NN_LOG_WRN("isp set %s=%d: errno %d", name, val, errno);
                return ESP_FAIL;
            }
            NN_LOG_INF("isp %s -> %d", name, val);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* Format the current ISP controls as "name=value\n..." (unsupported ones skipped). */
size_t nn_camera_isp_dump(char *buf, size_t cap)
{
    size_t o = 0;
    o += snprintf(buf + o, cap - o, "resolution=%dx%d format=H264 bitrate=%d gop=%d fps=%d\n",
                  WIDTH, HEIGHT, CONFIG_NN_CAMERA_H264_BITRATE,
                  CONFIG_NN_CAMERA_H264_GOP, CONFIG_NN_CAMERA_H264_FPS);
    for (int i = 0; i < (int)(sizeof s_isp_ctrls / sizeof s_isp_ctrls[0]) && o < cap; i++) {
        int32_t v;
        if (isp_get_ctrl(s_isp_ctrls[i].cid, &v) == 0)
            o += snprintf(buf + o, cap - o, "%s=%d\n", s_isp_ctrls[i].name, (int)v);
        else
            o += snprintf(buf + o, cap - o, "%s=(unsupported)\n", s_isp_ctrls[i].name);
    }
    return o;
}

static esp_err_t open_snapshot(void)
{
    s_snap_out = heap_caps_malloc(SNAP_MAX, MALLOC_CAP_SPIRAM);
    s_snap_sem = xSemaphoreCreateBinary();
    s_m2m_mtx = xSemaphoreCreateMutex();
    if (!s_snap_out || !s_snap_sem || !s_m2m_mtx) return ESP_ERR_NO_MEM;
    NN_LOG_INF("YUV snapshot ready (%dx%d I420, %d B)", WIDTH, HEIGHT, (int)SNAP_MAX);
    return ESP_OK;
}

esp_err_t nn_camera_capture_idr(const uint8_t **out, size_t *len, int timeout_ms)
{
    if (!s_snap_out) return ESP_ERR_NOT_SUPPORTED;
    /* Lock GOP=1 against the adaptive controllers.  esp_h264 latches a new GOP
     * only at the next IDR boundary, so the all-IDR regime starts up to one
     * old-GOP later — wait for the first NAL-verified IDR (capture loop checks
     * the NAL type) instead of assuming a fixed window. */
    s_snap_lock = true;
    /* NO encoder restart (each restart leaks internal RAM in the fork's stop/
     * start path — ENOMEM after a couple).  GOP is capped at 120, so a natural
     * NAL-verified IDR arrives within ~6 s; keep the bitrate modest during the
     * window so the IDR fits SNAP_MAX (adaptive control restores it after). */
    nn_camera_set_bitrate(1200000);
    s_snap_len = 0; s_want_snap = true;
    if (timeout_ms < 10000) timeout_ms = 10000;
    for (int waited = 0; waited < timeout_ms && s_snap_len == 0; waited += 100)
        vTaskDelay(pdMS_TO_TICKS(100));
    s_want_snap = false;
    s_snap_lock = false;
    nn_camera_set_gop(CONFIG_NN_CAMERA_H264_GOP);
    if (s_snap_len == 0) return ESP_FAIL;
    *out = s_snap_out; *len = s_snap_len;
    return ESP_OK;
}

/* Bits per pixel for the formats this driver can hand us.  Used to validate the
 * driver's own geometry: esp_video sizes the capture buffer as
 * width*height*out_bpp/8 (esp_video_csi_device.c) while the ISP's output colour
 * is programmed separately at STREAMON, so the two CAN disagree silently — and
 * when they do the ISP writes past the end of the buffer. */
static uint32_t fmt_bits_per_pixel(uint32_t fourcc)
{
    switch (fourcc) {
    case V4L2_PIX_FMT_SBGGR8:  case V4L2_PIX_FMT_GREY:    return 8;
    case V4L2_PIX_FMT_YUV420:                             return 12;
    case V4L2_PIX_FMT_RGB565:  case V4L2_PIX_FMT_YUV422P: return 16;
    case V4L2_PIX_FMT_RGB24:                              return 24;
    default:                                              return 0;
    }
}

/* Negotiate the format and (re)build the buffer ring on an OPEN fd.  Factored
 * out of open_capture() so a runtime format switch can rerun it after a
 * STREAMOFF without closing the device — esp_video does not fully re-arm the
 * CSI/ISP path on a bare close()+open(). */
static esp_err_t configure_capture(void)
{
    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        /* BT.709 limited range: H.264 decoders (ffmpeg, browsers) assume 709/
         * limited for HD by default; the ISP's defaults are 601/full, and the
         * mismatch shows as a constant purple/cool tint no WB tuning can fix. */
        .fmt.pix = { .width = CAP_W, .height = CAP_H, .pixelformat = s_cap_fmt,
                     .field = V4L2_FIELD_NONE,          /* progressive, not FIELD_ANY */
                     .colorspace = V4L2_COLORSPACE_REC709,
                     .ycbcr_enc = V4L2_YCBCR_ENC_709,
                     .quantization = V4L2_QUANTIZATION_LIM_RANGE },
    };
    if (ioctl(s_cap_fd, VIDIOC_S_FMT, &fmt) != 0) {
        NN_LOG_ERR("cam S_FMT %dx%d YUV420: errno %d — see cam fmt[] above",
                 CAP_W, CAP_H, errno);
        return ESP_FAIL;
    }
    /* VIDIOC_S_FMT is IN/OUT: the driver may substitute a different format and
     * still return success.  Verify it, because a silent substitution is
     * invisible everywhere else — the buffer is sized for what we ASKED for
     * while the ISP emits what it CHOSE, and every consumer downstream then
     * unpacks on the wrong grid (or the ISP overruns the buffer outright). */
    const uint32_t got = fmt.fmt.pix.pixelformat;
    NN_LOG_INF("cam S_FMT: %ux%u fmt='%c%c%c%c' bpl=%u sizeimage=%u",
               (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height,
               (int)(got & 0xff), (int)((got >> 8) & 0xff),
               (int)((got >> 16) & 0xff), (int)((got >> 24) & 0xff),
               (unsigned)fmt.fmt.pix.bytesperline, (unsigned)fmt.fmt.pix.sizeimage);
    if (got != s_cap_fmt) {
        NN_LOG_ERR("cam S_FMT SUBSTITUTED the format: asked '%c%c%c%c' got '%c%c%c%c'",
                   (int)(s_cap_fmt & 0xff), (int)((s_cap_fmt >> 8) & 0xff),
                   (int)((s_cap_fmt >> 16) & 0xff), (int)((s_cap_fmt >> 24) & 0xff),
                   (int)(got & 0xff), (int)((got >> 8) & 0xff),
                   (int)((got >> 16) & 0xff), (int)((got >> 24) & 0xff));
        return ESP_ERR_INVALID_STATE;
    }
    if (fmt.fmt.pix.width != CAP_W || fmt.fmt.pix.height != CAP_H) {
        NN_LOG_ERR("cam S_FMT resized: asked %dx%d got %ux%u", CAP_W, CAP_H,
                   (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height);
        return ESP_ERR_INVALID_STATE;
    }

    struct v4l2_requestbuffers req = {
        .count = CAP_BUFS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_cap_fd, VIDIOC_REQBUFS, &req) != 0) { NN_LOG_ERR("cam REQBUFS: %d", errno); return ESP_FAIL; }

    /* What the driver ALLOCATED vs what a frame of the negotiated format needs.
     * esp_video derives the buffer from out_bpp; the ISP's output colour is
     * programmed independently at STREAMON.  If they disagree the ISP DMAs past
     * the end of this allocation, so refuse to stream rather than corrupt the
     * heap. */
    const uint32_t bpp      = fmt_bits_per_pixel(s_cap_fmt);
    const size_t   need     = bpp ? ((size_t)CAP_W * CAP_H * bpp / 8) : 0;

    for (int i = 0; i < CAP_BUFS; i++) {
        struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
        if (ioctl(s_cap_fd, VIDIOC_QUERYBUF, &buf) != 0) { NN_LOG_ERR("cam QUERYBUF: %d", errno); return ESP_FAIL; }
        if (i == 0) {
            NN_LOG_INF("cam QUERYBUF: length=%u, a %dx%d %u-bpp frame needs %u",
                       (unsigned)buf.length, CAP_W, CAP_H, (unsigned)bpp, (unsigned)need);
        }
        if (need && buf.length < need) {
            NN_LOG_ERR("cam buffer TOO SMALL: %u B allocated, %u B per frame — "
                       "the ISP would DMA %u B past the end",
                       (unsigned)buf.length, (unsigned)need,
                       (unsigned)(need - buf.length));
            return ESP_ERR_INVALID_SIZE;
        }
        s_cap_buf[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_cap_fd, buf.m.offset);
        /* mmap reports failure as MAP_FAILED ((void *)-1), never NULL. */
        if (s_cap_buf[i] == MAP_FAILED || s_cap_buf[i] == NULL) {
            s_cap_buf[i] = NULL;
            NN_LOG_ERR("cam mmap %d failed (len %u)", i, (unsigned)buf.length);
            return ESP_ERR_NO_MEM;
        }
        if (ioctl(s_cap_fd, VIDIOC_QBUF, &buf) != 0) { NN_LOG_ERR("cam QBUF: %d", errno); return ESP_FAIL; }
    }
    return ESP_OK;
}

static esp_err_t open_capture(void)
{
#if CONFIG_NN_CAMERA_DIAG_ISP
    diag_load_format();
#endif
    s_cap_fd = open(CAM_DEV, O_RDWR);   /* RDWR so VIDIOC_S_CTRL (focus/VCM) is allowed */
    if (s_cap_fd < 0) { NN_LOG_ERR("open %s: errno %d", CAM_DEV, errno); return ESP_FAIL; }

    /* Discover what /dev/video0 actually offers (set up by esp_video from the
     * detected sensor). */
    struct v4l2_format cur = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(s_cap_fd, VIDIOC_G_FMT, &cur) == 0) {
        uint32_t pf = cur.fmt.pix.pixelformat;
        NN_LOG_INF("cam G_FMT: %ux%u fmt='%c%c%c%c'",
                 (unsigned)cur.fmt.pix.width, (unsigned)cur.fmt.pix.height,
                 (int)(pf & 0xff), (int)((pf >> 8) & 0xff),
                 (int)((pf >> 16) & 0xff), (int)((pf >> 24) & 0xff));
    }
    for (int i = 0; ; i++) {
        struct v4l2_fmtdesc fd = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        if (ioctl(s_cap_fd, VIDIOC_ENUM_FMT, &fd) != 0) break;
        uint32_t pf = fd.pixelformat;
        NN_LOG_INF("cam fmt[%d]: '%c%c%c%c' %s", i,
                 (int)(pf & 0xff), (int)((pf >> 8) & 0xff),
                 (int)((pf >> 16) & 0xff), (int)((pf >> 24) & 0xff), fd.description);
    }
    return configure_capture();
}

static esp_err_t open_encoder(void)
{
    s_m2m_fd = open(ENC_DEV, O_RDWR);
    if (s_m2m_fd < 0) { NN_LOG_ERR("open %s: errno %d", ENC_DEV, errno); return ESP_FAIL; }

    /* H.264 parameters. */
    set_ctrl(s_m2m_fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, CONFIG_NN_CAMERA_H264_GOP);
    set_ctrl(s_m2m_fd, V4L2_CID_MPEG_VIDEO_BITRATE,       CONFIG_NN_CAMERA_H264_BITRATE);
    set_ctrl(s_m2m_fd, V4L2_CID_MPEG_VIDEO_H264_MIN_QP,   CONFIG_NN_CAMERA_H264_MIN_QP);
    set_ctrl(s_m2m_fd, V4L2_CID_MPEG_VIDEO_H264_MAX_QP,   CONFIG_NN_CAMERA_H264_MAX_QP);

    /* Encoder OUTPUT = YUV420 frames we feed it (USERPTR -> camera buffers). */
    struct v4l2_format ofmt = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .fmt.pix = { .width = WIDTH, .height = HEIGHT, .pixelformat = CAP_FMT },
    };
    if (ioctl(s_m2m_fd, VIDIOC_S_FMT, &ofmt) != 0) { NN_LOG_ERR("enc OUT S_FMT: %d", errno); return ESP_FAIL; }
    struct v4l2_requestbuffers oreq = { .count = 1, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_USERPTR };
    if (ioctl(s_m2m_fd, VIDIOC_REQBUFS, &oreq) != 0) { NN_LOG_ERR("enc OUT REQBUFS: %d", errno); return ESP_FAIL; }

    /* Encoder CAPTURE = the H.264 bitstream. */
    struct v4l2_format cfmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix = { .width = WIDTH, .height = HEIGHT, .pixelformat = V4L2_PIX_FMT_H264 },
    };
    if (ioctl(s_m2m_fd, VIDIOC_S_FMT, &cfmt) != 0) { NN_LOG_ERR("enc CAP S_FMT: %d", errno); return ESP_FAIL; }
    struct v4l2_requestbuffers creq = { .count = 1, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (ioctl(s_m2m_fd, VIDIOC_REQBUFS, &creq) != 0) { NN_LOG_ERR("enc CAP REQBUFS: %d", errno); return ESP_FAIL; }

    struct v4l2_buffer cbuf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = 0 };
    if (ioctl(s_m2m_fd, VIDIOC_QUERYBUF, &cbuf) != 0) { NN_LOG_ERR("enc CAP QUERYBUF: %d", errno); return ESP_FAIL; }
    s_enc_buf = mmap(NULL, cbuf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_m2m_fd, cbuf.m.offset);
    if (s_enc_buf == NULL) { NN_LOG_ERR("enc mmap"); return ESP_ERR_NO_MEM; }
    if (ioctl(s_m2m_fd, VIDIOC_QBUF, &cbuf) != 0) { NN_LOG_ERR("enc CAP QBUF: %d", errno); return ESP_FAIL; }

    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_m2m_fd, VIDIOC_STREAMON, &t) != 0) { NN_LOG_ERR("enc CAP STREAMON: %d", errno); return ESP_FAIL; }
    t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(s_m2m_fd, VIDIOC_STREAMON, &t) != 0) { NN_LOG_ERR("enc OUT STREAMON: %d", errno); return ESP_FAIL; }
    return ESP_OK;
}

#if SCALE_EN
/* HW downscaler: PPA SRM, capture (CAP_WxCAP_H) I420 -> encoder (WIDTHxHEIGHT)
 * I420.  A scale (full FOV preserved), not a crop.  The output buffer is padded
 * to a 16-aligned height so the encoder's macroblock DMA can never read past
 * it, though esp_video sizes the encoder input at the exact WIDTHxHEIGHT. */
static esp_err_t open_scaler(void)
{
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    esp_err_t r = ppa_register_client(&pc, &s_ppa);
    if (r != ESP_OK) { NN_LOG_ERR("ppa_register_client: %s", esp_err_to_name(r)); return r; }

    /* ppa_core requires BOTH the output buffer address AND its buffer_size to be
     * aligned to the (external-RAM) cache line — query the exact value rather
     * than guess.  heap_caps_aligned_alloc aligns the address; round the size. */
    size_t al = 0;
    if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &al) != ESP_OK || al < 64) al = 128;
    #define PPA_SZ_ALIGN(x, a) (((size_t)(x) + (a) - 1) & ~((size_t)(a) - 1))
    s_mid_sz     = PPA_SZ_ALIGN((size_t)MID_W * MID_H * 3 / 2, al);   /* pass-1 output */
    s_scaled_len = (size_t)WIDTH * HEIGHT * 3 / 2;                    /* fed to encoder */
    s_scaled_sz  = PPA_SZ_ALIGN(s_scaled_len, al);                   /* pass-2 output  */
    s_mid_buf    = heap_caps_aligned_alloc(al, s_mid_sz,    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_scaled_buf = heap_caps_aligned_alloc(al, s_scaled_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    NN_LOG_INF("PPA cache align=%u  mid=%p/%u  scaled=%p/%u", (unsigned)al,
               s_mid_buf, (unsigned)s_mid_sz, s_scaled_buf, (unsigned)s_scaled_sz);
    if (!s_mid_buf || !s_scaled_buf) {
        NN_LOG_ERR("scaler buf alloc (%u + %u B)", (unsigned)s_mid_sz, (unsigned)s_scaled_sz);
        return ESP_ERR_NO_MEM;
    }
    memset(s_mid_buf, 0, s_mid_sz); memset(s_scaled_buf, 0, s_scaled_sz);
    NN_LOG_INF("PPA %dx%d -x7/8-> %dx%d -crop(%d,%d)-> %dx%d",
               CAP_W, CAP_H, MID_W, MID_H, CROP_X, CROP_Y, WIDTH, HEIGHT);
    return ESP_OK;
}

/* Two-pass PPA: x0.875 downscale (exactly representable in the PPA's 1/16 scale
 * -> no unwritten margin) then a minimal center-crop to the exact encoder size.
 * Returns false on error. */
static bool scale_frame(const uint8_t *src)
{
    const ppa_color_range_t rng = PPA_COLOR_RANGE_LIMIT;
    const ppa_color_conv_std_rgb_yuv_t std = PPA_COLOR_CONV_STD_RGB_YUV_BT709;
#if DIAG_CROP_ONLY
    /* Single crop (scale 1.0), no scale pass.  Production use: the sensor sends
     * NN_CAMERA_CROP_X_OFFSET extra LEFT columns so the line-start chroma
     * artifact (purple band, sensor/ISP border) lands in the discarded margin —
     * offset all the way left-biased when configured, centred otherwise. */
    ppa_srm_oper_config_t cc = {
        .in  = { .buffer = src, .pic_w = CAP_W, .pic_h = CAP_H,
                 .block_w = WIDTH, .block_h = HEIGHT,
#if CONFIG_NN_CAMERA_CROP_X_OFFSET >= 0
                 .block_offset_x = (CONFIG_NN_CAMERA_CROP_X_OFFSET) & ~1,
#else
                 .block_offset_x = ((CAP_W - WIDTH) / 2) & ~1,
#endif
                 .block_offset_y = ((CAP_H - HEIGHT) / 2) & ~1,
                 .srm_cm = PPA_SRM_COLOR_MODE_YUV420, .yuv_range = rng, .yuv_std = std },
        .out = { .buffer = s_scaled_buf, .buffer_size = s_scaled_sz, .pic_w = WIDTH, .pic_h = HEIGHT,
                 .srm_cm = PPA_SRM_COLOR_MODE_YUV420, .yuv_range = rng, .yuv_std = std },
        .scale_x = 1.0f, .scale_y = 1.0f,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0, .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(s_ppa, &cc) != ESP_OK) { NN_LOG_WRN("ppa crop"); return false; }
    return true;
#endif
    /* pass 1: CAP -> MID (full frame, x0.875) */
    ppa_srm_oper_config_t p1 = {
        .in  = { .buffer = src, .pic_w = CAP_W, .pic_h = CAP_H, .block_w = CAP_W, .block_h = CAP_H,
                 .srm_cm = PPA_SRM_COLOR_MODE_YUV420, .yuv_range = rng, .yuv_std = std },
        .out = { .buffer = s_mid_buf, .buffer_size = s_mid_sz, .pic_w = MID_W, .pic_h = MID_H,
                 .srm_cm = PPA_SRM_COLOR_MODE_YUV420, .yuv_range = rng, .yuv_std = std },
        .scale_x = 0.875f, .scale_y = 0.875f,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0, .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(s_ppa, &p1) != ESP_OK) { NN_LOG_WRN("ppa p1"); return false; }
    /* pass 2: center-crop MID -> WIDTHxHEIGHT (scale 1.0) */
    ppa_srm_oper_config_t p2 = {
        .in  = { .buffer = s_mid_buf, .pic_w = MID_W, .pic_h = MID_H,
                 .block_w = WIDTH, .block_h = HEIGHT, .block_offset_x = CROP_X, .block_offset_y = CROP_Y,
                 .srm_cm = PPA_SRM_COLOR_MODE_YUV420, .yuv_range = rng, .yuv_std = std },
        .out = { .buffer = s_scaled_buf, .buffer_size = s_scaled_sz, .pic_w = WIDTH, .pic_h = HEIGHT,
                 .srm_cm = PPA_SRM_COLOR_MODE_YUV420, .yuv_range = rng, .yuv_std = std },
        .scale_x = 1.0f, .scale_y = 1.0f,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0, .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(s_ppa, &p2) != ESP_OK) { NN_LOG_WRN("ppa p2"); return false; }
    return true;
}
#endif /* SCALE_EN */

#if CONFIG_NN_CAMERA_DIAG_ISP
/* ── ISP diagnostic mode ──────────────────────────────────────────────────
 * Push raw RGB frames (+ timestamp + the live ISP settings) to the media
 * service's /diag/isp WebSocket instead of encoding H.264 — so the host can
 * see exactly what the ISP produces, bypassing the PPA and the encoder. */
#define DIAG_CHUNK (16 * 1024)

/* {device,ts,w,h,fmt,bytes,settings:{...}} — settings = the same ISP knobs the
 * normal pipeline exposes via `isp`. */
static int diag_build_meta(char *out, size_t cap, int w, int h, size_t bytes)
{
    int64_t ts_ms = (int64_t)time(NULL) * 1000;
    if (ts_ms < 1000000000000LL) ts_ms = esp_timer_get_time() / 1000; /* clock unsynced -> uptime ms */
    int o = snprintf(out, cap,
        "{\"device\":\"%s\",\"ts\":%lld,\"w\":%d,\"h\":%d,\"fmt\":\"%s\",\"bytes\":%u,"
        "\"settings\":{\"resolution\":\"%dx%d\",\"sensor\":\"%s\",\"fps\":%d,\"wb_r\":%.2f,\"wb_b\":%.2f",
        CONFIG_NN_CAMERA_DIAG_NAME, (long long)ts_ms, w, h, DIAG_FMT_STR, (unsigned)bytes,
        w, h, NN_CAM_SENSOR_NAME, CONFIG_NN_CAMERA_H264_FPS, s_wb_r, s_wb_b);
    for (int i = 0; i < (int)(sizeof s_isp_ctrls / sizeof s_isp_ctrls[0]) && o < (int)cap - 40; i++) {
        int32_t v;
        if (isp_get_ctrl(s_isp_ctrls[i].cid, &v) == 0)
            o += snprintf(out + o, cap - o, ",\"%s\":%d", s_isp_ctrls[i].name, (int)v);
    }
    o += snprintf(out + o, cap - o, "}}");
    return o;
}

static void diag_task(void *arg)
{
    char uri[96];
    snprintf(uri, sizeof uri, "ws://%s:%d/diag/isp",
             CONFIG_NN_CAMERA_DIAG_HOST, CONFIG_NN_CAMERA_DIAG_PORT);
    /* no host configured (CONFIG_NN_CAMERA_DIAG_HOST empty) = capture only */
    void *cli = CONFIG_NN_CAMERA_DIAG_HOST[0] ? nn_cam_diag_open(uri) : NULL;
    /* The hub WebSocket is one CONSUMER, not the pipeline.  It used to be fatal
     * here — and since this task restarts on every `isp fmt` switch, a single
     * failed handshake (hub down, `isp ws 0` intended anyway) silently killed
     * the capture loop and every later `isp send` timed out.  Run without it. */
    if (!cli) NN_LOG_WRN("diag ws unavailable — capture continues, hub push off");
    NN_LOG_INF("ISP diag: RGB888 %dx%d -> %s every %d ms",
               CAP_W, CAP_H, uri, CONFIG_NN_CAMERA_DIAG_INTERVAL_MS);

    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cap_fd, VIDIOC_STREAMON, &t) != 0) {
        NN_LOG_ERR("diag cam STREAMON: %d", errno);
        nn_cam_diag_close(cli); s_running = false; vTaskDelete(NULL); return;
    }
    apply_wb_retry();

    static char meta[900];
    while (s_running) {
        struct v4l2_buffer cap = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(s_cap_fd, VIDIOC_DQBUF, &cap) != 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        s_stats.csi_done++;
        if (s_diag_want && !s_diag_len) {
            /* Pipeline tap.  PREFERRED PATH: copy the frame into a private
             * buffer and re-queue immediately, so the tap observes exactly the
             * bytes the encoder will see.  Holding the mmap'd buffer across a
             * multi-second network send instead let the pipeline keep running
             * underneath it, and the frame the host received did not decode as
             * the negotiated format at all.  Fall back to borrow-and-hold only
             * if the copy cannot be allocated. */
            if (!s_diag_copy && cap.bytesused) {
                s_diag_copy = heap_caps_malloc(cap.bytesused, MALLOC_CAP_SPIRAM);
                s_diag_copy_sz = s_diag_copy ? cap.bytesused : 0;
            }
            if (s_diag_copy && cap.bytesused <= s_diag_copy_sz) {
                /* INVALIDATE before the CPU reads it.  The ISP fills this buffer
                 * by DMA; the H.264 encoder also reads it by DMA and therefore
                 * sees the true bytes, which is why the live stream is correct.
                 * A plain CPU memcpy can hit stale cache lines instead, and the
                 * frame the host received then did not decode as the negotiated
                 * format at all. */
                /* NOTE: esp_cache REJECTS the UNALIGNED flag for M2C outright
                 * ("M2C direction doesn't allow UNALIGNED", logged every
                 * capture) — so this invalidate had NEVER actually run and the
                 * copy could read stale cache.  The buffers are driver-aligned
                 * (esp_video allocates to esp_cache_get_alignment) and every
                 * frame size we use is a multiple of the 64 B line, so the
                 * plain call is legal.  Round bytesused up defensively. */
                esp_cache_msync(s_cap_buf[cap.index], (cap.bytesused + 63) & ~63u,
                                ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                memcpy(s_diag_copy, s_cap_buf[cap.index], cap.bytesused);
                s_diag_buf = s_diag_copy;
                s_diag_len = cap.bytesused;          /* loop continues normally */
            } else {
                s_diag_buf = s_cap_buf[cap.index];
                s_diag_len = cap.bytesused;
                for (int w = 0; w < 3000 && s_diag_want; w++) vTaskDelay(pdMS_TO_TICKS(10));
                s_diag_len = 0;
                s_diag_buf = NULL;
            }
        }
        size_t len = (size_t)CAP_W * CAP_H * s_diag_bpp;
        /* An armed tap (`isp send`) OUTRANKS the WebSocket sink.  Pushing 7.46 MB
         * to the hub takes many seconds (8 s timeout per chunk) and, while it is
         * in flight, this loop never returns to the tap check above — which is
         * exactly why `isp send` timed out at 30 s with the loop apparently
         * "running".  Skip the push whenever an operator is waiting. */
        if (cli && s_diag_ws_on && nn_cam_diag_connected(cli) && !s_diag_want) {
            int ml = diag_build_meta(meta, sizeof meta, CAP_W, CAP_H, len);
            const uint8_t *p = s_cap_buf[cap.index];
            bool ok = nn_cam_diag_text(cli, meta, ml, 4000) >= 0;
            for (size_t off = 0; ok && off < len && s_running && !s_diag_want; off += DIAG_CHUNK) {
                size_t n = (len - off) < DIAG_CHUNK ? (len - off) : DIAG_CHUNK;
                if (nn_cam_diag_bin(cli, p + off, (int)n, 8000) < 0) {
                    NN_LOG_WRN("diag send abort @%u/%u", (unsigned)off, (unsigned)len);
                    ok = false;
                }
            }
            if (s_diag_want) {
                ok = false;                      /* yielded to the tap, not an error */
            }
            if (ok) { s_stats.frames++; NN_LOG_INF("diag frame sent (%u B)", (unsigned)len); }
        }
        if (s_fill) poison_buffer(s_cap_buf[cap.index], cap.length ? cap.length : (size_t)CAP_W * CAP_H * 3);
        ioctl(s_cap_fd, VIDIOC_QBUF, &cap);
        /* Sliced sleep so a stop request (format switch) is honoured within
         * ~100 ms instead of after the full interval. */
        for (int t = 0; t < CONFIG_NN_CAMERA_DIAG_INTERVAL_MS && s_running; t += 100)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (cli) nn_cam_diag_close(cli);
    s_diag_task_gone = true;
    vTaskDelete(NULL);
}

/* M2M support: grab one RAW frame into the caller's buffer, then take the
 * whole esp_video pipeline down so the M2M rig can own the ISP and CSI bridge.
 * Only valid in the RAW diag boot mode. */
esp_err_t nn_camera__m2m_shutdown(uint8_t *dst, size_t dst_len)
{
    if (s_cap_fmt != V4L2_PIX_FMT_SBGGR8) return ESP_ERR_INVALID_STATE;
    if (dst_len < (size_t)CAP_W * CAP_H)  return ESP_ERR_INVALID_SIZE;

    nn_camera__diag_arm();
    for (int i = 0; i < 2000 && !nn_camera__diag_len(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    const uint32_t len = nn_camera__diag_len();
    if (!len || len > dst_len) { nn_camera__diag_disarm(); return ESP_ERR_TIMEOUT; }
    memcpy(dst, nn_camera__diag_buf(), len);
    nn_camera__diag_disarm();

    s_diag_task_gone = false;
    s_running = false;
    for (int i = 0; i < 3000 && !s_diag_task_gone; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (!s_diag_task_gone) return ESP_ERR_TIMEOUT;
    int ty = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_cap_fd, VIDIOC_STREAMOFF, &ty);
    close(s_cap_fd);
    s_cap_fd = -1;
    NN_LOG_INF("pipeline down; RAW frame snapped (%u B)", (unsigned)len);
    return ESP_OK;
}

/* Select the capture format for the NEXT boot and restart.
 *
 * A LIVE switch (STREAMOFF -> S_FMT -> REQBUFS -> STREAMON on the open fd) was
 * tried first and delivers exactly ONE frame before the pipeline stalls —
 * esp_video's buffer bookkeeping does not survive a second REQBUFS.  A bare
 * close()+reopen() delivers none.  Rather than patch esp_video internals, use
 * the one path that is exercised on every boot: persist the choice in NVS and
 * esp_restart().  This is also better SCIENCE for the interleave experiment:
 * every sample, raw or rgb, is taken after an IDENTICAL soft-reboot, so the
 * reset-path variable that confounded the earlier RAW-vs-RGB comparison is
 * held constant by construction. */
esp_err_t nn_camera_diag_set_format(const char *name)
{
    uint32_t fmt;
    if      (strcmp(name, "raw") == 0) fmt = V4L2_PIX_FMT_SBGGR8;
    else if (strcmp(name, "rgb") == 0) fmt = V4L2_PIX_FMT_RGB24;
    else return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t r = nvs_open("nn_cam_diag", NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_set_u32(h, "cap_fmt", fmt);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    if (r != ESP_OK) return r;
    NN_LOG_INF("capture format '%s' persisted — rebooting", name);
    vTaskDelay(pdMS_TO_TICKS(200));      /* let the console line flush */
    esp_restart();
    return ESP_OK;                        /* not reached */
}

/* At init: adopt a persisted format choice, if any. */
static void diag_load_format(void)
{
    nvs_handle_t h;
    if (nvs_open("nn_cam_diag", NVS_READONLY, &h) != ESP_OK) return;
    uint32_t fmt = 0;
    if (nvs_get_u32(h, "cap_fmt", &fmt) == ESP_OK &&
            (fmt == V4L2_PIX_FMT_SBGGR8 || fmt == V4L2_PIX_FMT_RGB24)) {
        s_cap_fmt  = fmt;
        s_diag_bpp = (fmt == V4L2_PIX_FMT_RGB24) ? 3 : 1;
        NN_LOG_INF("diag capture format from NVS: '%c%c%c%c'",
                   (int)(fmt & 0xff), (int)((fmt >> 8) & 0xff),
                   (int)((fmt >> 16) & 0xff), (int)((fmt >> 24) & 0xff));
    }
    nvs_close(h);
}
#else  /* !CONFIG_NN_CAMERA_DIAG_ISP */
esp_err_t nn_camera_diag_set_format(const char *name)
{
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;   /* runtime format switching is a DIAG facility */
}
#endif /* CONFIG_NN_CAMERA_DIAG_ISP */

/* ── Gray-world auto white balance ────────────────────────────────────────
 * The fork removed the IPA AWB, so the ISP collects white-patch R/G/B sums (on
 * /dev/video20, ESP_VIDEO_ISP_STATS meta) but nothing applies them.  Read them
 * and drive the CCM gains so R,G,B means equalise — self-adjusts to any room
 * (manual gains can't: AE swings the exposure and the CCM-column scaling maps
 * non-linearly to output R/B). */
static void awb_task(void *arg)
{
    int fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (fd < 0) { NN_LOG_ERR("awb open: errno %d", errno); vTaskDelete(NULL); return; }

    /* Wide white-patch range => averages most of the frame (gray-world). */
    esp_video_isp_awb_t awb = { .enable = true, .green_max = 250, .green_min = 15,
                                .rg_max = 3.0f, .rg_min = 0.2f, .bg_max = 3.0f, .bg_min = 0.2f };
    struct v4l2_ext_control ec = { .id = V4L2_CID_USER_ESP_ISP_AWB, .size = sizeof awb, .p_u8 = (void *)&awb };
    struct v4l2_ext_controls ecs = { .ctrl_class = V4L2_CTRL_CLASS_USER, .count = 1, .controls = &ec };
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ecs) != 0) NN_LOG_WRN("awb cfg: errno %d", errno);

    struct v4l2_format f = { .type = V4L2_BUF_TYPE_META_CAPTURE };
    f.fmt.meta.dataformat = V4L2_META_FMT_ESP_ISP_STATS;
    ioctl(fd, VIDIOC_S_FMT, &f);
    struct v4l2_requestbuffers rb = { .count = 2, .type = V4L2_BUF_TYPE_META_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) != 0) { NN_LOG_ERR("awb reqbufs: %d", errno); close(fd); vTaskDelete(NULL); return; }
    void *mb[2] = { 0 };
    for (int i = 0; i < 2; i++) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_META_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
        if (ioctl(fd, VIDIOC_QUERYBUF, &b) != 0) { close(fd); vTaskDelete(NULL); return; }
        mb[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        ioctl(fd, VIDIOC_QBUF, &b);
    }
    int t = V4L2_BUF_TYPE_META_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &t) != 0) { NN_LOG_ERR("awb streamon: %d", errno); close(fd); vTaskDelete(NULL); return; }
    NN_LOG_INF("gray-world AWB running (ISP stats)");

    float rg = s_wb_r, bg = s_wb_b;
    while (s_running) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_META_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        const esp_video_isp_stats_t *st = (const esp_video_isp_stats_t *)mb[b.index];
        if (st && (st->flags & ESP_VIDEO_ISP_STATS_FLAG_AWB)) {
            uint32_t nw = st->awb.awb_result.white_patch_num;
            float sr = (float)st->awb.awb_result.sum_r, sg = (float)st->awb.awb_result.sum_g,
                  sb = (float)st->awb.awb_result.sum_b;
            if (nw > 200 && sr > 1 && sb > 1) {
                float tr = sg / sr, tb = sg / sb;             /* gray-world targets */
                if (tr < 0.4f) tr = 0.4f;
                else if (tr > 2.5f) tr = 2.5f;
                if (tb < 0.4f) tb = 0.4f;
                else if (tb > 2.5f) tb = 2.5f;
                rg = 0.85f * rg + 0.15f * tr;                 /* low-pass: no flicker */
                bg = 0.85f * bg + 0.15f * tb;
                nn_camera_set_wb(rg, bg);
            }
        }
        ioctl(fd, VIDIOC_QBUF, &b);
        vTaskDelay(pdMS_TO_TICKS(300));                       /* ~3 Hz */
    }
    ioctl(fd, VIDIOC_STREAMOFF, &t);
    close(fd);
    vTaskDelete(NULL);
}

esp_err_t nn_camera_init(void)
{
    s_stats.width = WIDTH;
    s_stats.height = HEIGHT;

    esp_err_t ret = esp_video_init(&s_video_cfg);
    if (ret != ESP_OK) { NN_LOG_ERR("esp_video_init: %s", esp_err_to_name(ret)); return ret; }

    ret = open_capture();
    if (ret != ESP_OK) return ret;
#if CONFIG_NN_CAMERA_DIAG_ISP
    NN_LOG_INF("ISP DIAG build: RGB capture only, no encoder/scaler");
    return ESP_OK;      /* diag: raw RGB capture only — no encoder/scaler/snapshot */
#else
    ret = open_encoder();
    if (ret != ESP_OK) return ret;
#if SCALE_EN
    ret = open_scaler();
    if (ret != ESP_OK) return ret;
#endif
    open_snapshot();   /* raw YUV snapshot buffer for ISP tuning */
#endif

    NN_LOG_INF("init ok: %dx%d YUV420 -> H.264 (gop %d, %d bps) via esp_video",
             WIDTH, HEIGHT, CONFIG_NN_CAMERA_H264_GOP, CONFIG_NN_CAMERA_H264_BITRATE);
    return ESP_OK;
}


static void capture_task(void *arg)
{
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cap_fd, VIDIOC_STREAMON, &t) != 0) {
        NN_LOG_ERR("cam STREAMON: %d", errno);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    NN_LOG_INF("capture+encode running (%dx%d)", WIDTH, HEIGHT);
    /* Static white balance via the ISP CCM (IPA awb removed — its gains were
     * capped/opaque and manual V4L2 balance never reached the hardware).
     * Tuned empirically; runtime-trim with `isp wb <r> <b>`. */
    apply_wb_retry();
    /* Neutralise the ISP's WB block instead of fighting it.  History of the
     * purple left-edge band (bisected 2026-08-03/04):
     *   - the WB block manufactures a 16-32 px chroma band at every line start,
     *     error scaling with the correction it applies (defaults red_balance=400
     *     blue_balance=280 — i.e. x0.4/x0.28 attenuation under our CCM WB);
     *   - DISABLING the block works momentarily but the IPA re-enables it
     *     seconds later, and a periodic re-disable races the IPA through
     *     garbage states (alternating magenta/green stripes, worse than the
     *     original artifact);
     *   - setting the balances to UNITY (1000/1000) STICKS across IPA activity
     *     — the IPA re-enables the block but never rewrites the values — and an
     *     identity multiply produces zero line-start error.
     * So: leave the block alone, make its gains unity, keep real WB in the CCM
     * (NN_CAMERA_WB_{R,B}_MILLI, tuned for exactly this configuration). */
    /* LEFT-EDGE PURPLE BAND: the IPA is NOT the cause — do not hold it here.
     *
     * A previous revision froze SHARPEN here, on the strength of five
     * consecutive clean captures.  That was under-powered: the band appears in
     * roughly half of all frames, so five cleans in a row happen by chance
     * about 1 time in 32.  An interleaved 16-capture experiment (2026-08-04)
     * settled it — freezing EVERY IPA block (`isp hold 31`: BF, demosaic,
     * sharpen, gamma, CCM) did not remove the band at all:
     *     hold=31 (all IPA frozen) : 6/8 frames banded, mean width 38.9px
     *     hold=4  (sharpen only)   : 2/8 frames banded, mean width 16.2px
     * If the IPA produced the band, freezing all of it would eliminate it.  It
     * does not, so the fault lies below the IPA — in the ISP pipeline itself or
     * in the sensor data reaching it.  (The 6/8 vs 2/8 split is NOT itself
     * significant, Fisher p~0.13; the load-bearing result is that neither
     * setting eliminates the band.)
     *
     * Also ruled out: WB/CCM (survives unity WB), sensor DIG-CROP (X_OFFSET is
     * correctly 192, centred), buffer overrun (QUERYBUF matches exactly), and a
     * wrap of the right-hand image into the left edge (correlates +0.009 with
     * the right edge vs +0.006 for an arbitrary strip; the band is random
     * speckle, not displaced scene content). */

    while (s_running) {
        /* 1. dequeue a captured camera frame (YUV420) */
        struct v4l2_buffer cap = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(s_cap_fd, VIDIOC_DQBUF, &cap) != 0) {
            NN_LOG_WRN("cam DQBUF: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (s_diag_want && !s_diag_len) {
            /* Pipeline tap.  PREFERRED PATH: copy the frame into a private
             * buffer and re-queue immediately, so the tap observes exactly the
             * bytes the encoder will see.  Holding the mmap'd buffer across a
             * multi-second network send instead let the pipeline keep running
             * underneath it, and the frame the host received did not decode as
             * the negotiated format at all.  Fall back to borrow-and-hold only
             * if the copy cannot be allocated. */
            if (!s_diag_copy && cap.bytesused) {
                s_diag_copy = heap_caps_malloc(cap.bytesused, MALLOC_CAP_SPIRAM);
                s_diag_copy_sz = s_diag_copy ? cap.bytesused : 0;
            }
            if (s_diag_copy && cap.bytesused <= s_diag_copy_sz) {
                /* INVALIDATE before the CPU reads it.  The ISP fills this buffer
                 * by DMA; the H.264 encoder also reads it by DMA and therefore
                 * sees the true bytes, which is why the live stream is correct.
                 * A plain CPU memcpy can hit stale cache lines instead, and the
                 * frame the host received then did not decode as the negotiated
                 * format at all. */
                /* NOTE: esp_cache REJECTS the UNALIGNED flag for M2C outright
                 * ("M2C direction doesn't allow UNALIGNED", logged every
                 * capture) — so this invalidate had NEVER actually run and the
                 * copy could read stale cache.  The buffers are driver-aligned
                 * (esp_video allocates to esp_cache_get_alignment) and every
                 * frame size we use is a multiple of the 64 B line, so the
                 * plain call is legal.  Round bytesused up defensively. */
                esp_cache_msync(s_cap_buf[cap.index], (cap.bytesused + 63) & ~63u,
                                ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                memcpy(s_diag_copy, s_cap_buf[cap.index], cap.bytesused);
                s_diag_buf = s_diag_copy;
                s_diag_len = cap.bytesused;          /* loop continues normally */
            } else {
                s_diag_buf = s_cap_buf[cap.index];
                s_diag_len = cap.bytesused;
                for (int w = 0; w < 3000 && s_diag_want; w++) vTaskDelay(pdMS_TO_TICKS(10));
                s_diag_len = 0;
                s_diag_buf = NULL;
            }
        }
        s_stats.csi_done++;          /* a real camera frame arrived */

        /* fps divisor: encode 1 of every s_fps_divisor captured frames; recycle
         * the rest without touching the encoder (cheap frame-rate reduction). */
        if (s_fps_divisor > 1 && (s_stats.csi_done % s_fps_divisor) != 0) {
            if (s_fill) poison_buffer(s_cap_buf[cap.index], cap.length ? cap.length : (size_t)CAP_W * CAP_H * 3);
            ioctl(s_cap_fd, VIDIOC_QBUF, &cap);
            continue;
        }

        /* encoder restart => next frame is an IDR.  Used by the snapshot path
         * (with GOP=1) and by nn_camera_force_idr() (viewer joined: give the
         * stream a fresh decode point now instead of one GOP later).
         * Serialized against live ctrls and rate-limited to one per 2 s (N
         * viewers joining at once must not restart-storm the encoder). */
        static int64_t s_last_restart_us;
        if (s_snap_restart &&
                (esp_timer_get_time() - s_last_restart_us > 2000000 || s_want_snap)) {
            s_snap_restart = false;
            s_last_restart_us = esp_timer_get_time();
            xSemaphoreTake(s_m2m_mtx, portMAX_DELAY);
            int tc = V4L2_BUF_TYPE_VIDEO_CAPTURE, to = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            ioctl(s_m2m_fd, VIDIOC_STREAMOFF, &to);
            ioctl(s_m2m_fd, VIDIOC_STREAMOFF, &tc);
            struct v4l2_buffer eb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                      .memory = V4L2_MEMORY_MMAP, .index = 0 };
            ioctl(s_m2m_fd, VIDIOC_QBUF, &eb);
            if (ioctl(s_m2m_fd, VIDIOC_STREAMON, &tc) != 0 ||
                    ioctl(s_m2m_fd, VIDIOC_STREAMON, &to) != 0) {
                NN_LOG_WRN("snap: encoder restart failed: errno %d", errno);
                /* restart alloc failed (heap fragmentation) — back way off so a
                 * failing restart can't be retried into a spam loop */
                s_last_restart_us = esp_timer_get_time() + 28000000;
            }
            xSemaphoreGive(s_m2m_mtx);
        } else {
            s_snap_restart = false;   /* rate-limited: drop the extra request */
        }

        /* 2. feed it to the encoder OUTPUT queue (zero-copy via USERPTR).  When
         *    the sensor mode is larger than the encoder size, PPA-downscale the
         *    captured frame first and feed the scaled buffer instead. */
#if SCALE_EN
        if (s_fill) poison_buffer(s_cap_buf[cap.index], cap.length ? cap.length : (size_t)CAP_W * CAP_H * 3);
        if (!scale_frame(s_cap_buf[cap.index])) { ioctl(s_cap_fd, VIDIOC_QBUF, &cap); continue; }
        struct v4l2_buffer mo = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_USERPTR, .index = 0,
            .m.userptr = (unsigned long)s_scaled_buf, .length = s_scaled_len,
        };
#else
        struct v4l2_buffer mo = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_USERPTR, .index = 0,
            .m.userptr = (unsigned long)s_cap_buf[cap.index], .length = cap.bytesused,
        };
#endif
        ioctl(s_m2m_fd, VIDIOC_QBUF, &mo);

        /* 3. dequeue the encoded H.264 frame */
        struct v4l2_buffer mc = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(s_m2m_fd, VIDIOC_DQBUF, &mc) == 0) {
            bool key = (mc.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
            /* self-heal: >2 s of zero-byte output = encoder wedged; restart it */
            static uint32_t s_zero_run;
            if (mc.bytesused == 0) {
                if (++s_zero_run == 60) { s_snap_restart = true; s_zero_run = 0;
                                          NN_LOG_WRN("encoder emitting 0-byte frames — restarting"); }
            } else {
                s_zero_run = 0;
            }
            s_stats.frames++;
            if (key) s_stats.keyframes++;
            s_stats.last_size = mc.bytesused;
            s_stats.total_bytes += mc.bytesused;
            if (s_cb) s_cb(s_enc_buf, mc.bytesused, key, s_cb_ctx);
            /* on-demand IDR snapshot: accept the first access unit that starts
             * with SPS/PPS/IDR (NAL 7/8/5) — the encoder doesn't set the V4L2
             * KEYFRAME flag, and frame size alone can't tell a big P-frame from
             * an IDR, so verify the NAL type directly. */
            (void)key;
            if (s_want_snap && s_snap_out && s_snap_len == 0 && mc.bytesused > 5) {
                const uint8_t *b = s_enc_buf;
                size_t skip = (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) ? 4
                            : (b[0] == 0 && b[1] == 0 && b[2] == 1)              ? 3 : 0;
                uint8_t nal = skip ? (b[skip] & 0x1F) : 0;
                if ((nal == 5 || nal == 7 || nal == 8) && mc.bytesused <= SNAP_MAX) {
                    memcpy(s_snap_out, s_enc_buf, mc.bytesused);   /* whole IDR only */
                    s_snap_len = mc.bytesused;
                }
            }
            ioctl(s_m2m_fd, VIDIOC_QBUF, &mc);       /* recycle encoder CAPTURE buf */
        } else {
            s_stats.drops++;
        }

        /* 4. recycle the camera + encoder OUTPUT buffers */
        if (s_fill) poison_buffer(s_cap_buf[cap.index], cap.length ? cap.length : (size_t)CAP_W * CAP_H * 3);
        ioctl(s_cap_fd, VIDIOC_QBUF, &cap);
        ioctl(s_m2m_fd, VIDIOC_DQBUF, &mo);
    }
    vTaskDelete(NULL);
}

esp_err_t nn_camera_start(void)
{
    if (s_running) return ESP_OK;
    if (s_cap_fd < 0) return ESP_ERR_INVALID_STATE;
    s_running = true;
#if CONFIG_NN_CAMERA_DIAG_ISP
    if (xTaskCreate(diag_task, "cam_diag", 8192, NULL, 11, &s_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
#else
    if (xTaskCreate(capture_task, "nn_cam", 6144, NULL, 12, &s_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
#endif
    /* gray-world AWB disabled: the CCM-column WB over-amplifies (clips to white);
     * needs a luma-anchored loop / proper pre-CCM diagonal WB.  Fixed WB for now. */
    /* xTaskCreate(awb_task, "cam_awb", 5120, NULL, 10, NULL); */
    (void)awb_task;
    return ESP_OK;
}

esp_err_t nn_camera_stop(void)
{
    if (!s_running) return ESP_OK;
    s_running = false;
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (s_cap_fd >= 0) ioctl(s_cap_fd, VIDIOC_STREAMOFF, &t);
    return ESP_OK;
}
