/* Pipeline tap: push one CAPTURE-device frame to a TCP receiver.
 *
 * The P4 ISP is fixed-function with a single DMA output, so the pipeline can
 * only be observed at its tail.  Which DOMAIN that tail sits in is chosen by
 * the capture pixel format (esp_video keeps the ISP in-path for SBGGR8 /
 * RGB565 / RGB24 / YUV420 / YUV422P):
 *
 *   SBGGR8 -> Bayer domain   : before demosaic / WBG / CCM / gamma / sharpen
 *   RGB24  -> RGB domain     : after the full RGB chain, before RGB2YUV
 *   YUV420 -> production tail: everything applied
 *
 * Per-STAGE contribution therefore comes from DIFFERENCING captures taken with
 * individual blocks disabled (`isp blk <name> 0`), not from tapping mid-chain.
 *
 * Header fields come from the driver's own G_FMT, so whatever stride and slice
 * height the pipeline actually uses are reported rather than assumed.
 */
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stddef.h>
#include "linux/videodev2.h"
#include "esp_video_device.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_cam_tap);

#include "nn_camera_diag.h"
#include "../src/nn_camera_internal.h"

esp_err_t nn_camera_diag_send(const char *host, uint16_t port, const char *desc)
{
    const int fd = nn_camera__cap_fd();
    if (fd < 0) return ESP_ERR_INVALID_STATE;

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
        NN_LOG_ERR("G_FMT: errno %d", errno);
        return ESP_FAIL;
    }
    const uint32_t w       = fmt.fmt.pix.width;
    const uint32_t h       = fmt.fmt.pix.height;
    uint32_t       fourcc  = fmt.fmt.pix.pixelformat;
    uint32_t       stride  = fmt.fmt.pix.bytesperline;
    const uint32_t sizeimg = fmt.fmt.pix.sizeimage;

    /* The header must carry BYTES per row.  esp_video under-reports here: for
     * RGB24 1920x1296 it returns bytesperline=1920 (a PIXEL count) and
     * sizeimage=0, which would tell the receiver each row is 1920 B when it is
     * really 5760 — a 3x skew that looks like a corrupt sensor readout.  Take
     * the driver's value only when it is at least the packed minimum. */
    static const struct { uint32_t fourcc; uint32_t num, den; } bpp_tbl[] = {
        { V4L2_PIX_FMT_RGB24,   3, 1 }, { V4L2_PIX_FMT_RGB565,  2, 1 },
        { V4L2_PIX_FMT_SBGGR8,  1, 1 }, { V4L2_PIX_FMT_GREY,    1, 1 },
        { V4L2_PIX_FMT_YUV420,  1, 1 }, { V4L2_PIX_FMT_YUV422P, 1, 1 },
    };
    uint32_t min_stride = 0;
    for (size_t i = 0; i < sizeof bpp_tbl / sizeof bpp_tbl[0]; i++) {
        if (bpp_tbl[i].fourcc == fourcc) {
            min_stride = w * bpp_tbl[i].num / bpp_tbl[i].den;
            break;
        }
    }
    if (stride < min_stride) {
        NN_LOG_WRN("driver bytesperline=%u < packed %u for %.4s — reporting %u",
                   (unsigned)stride, (unsigned)min_stride,
                   (const char *)&fourcc, (unsigned)min_stride);
        stride = min_stride;
    }
    if (!stride) stride = w;                       /* unknown format, last resort */

    /* slice_height is the PADDED row count (planar chroma starts at
     * stride*slice_height).  sizeimage is 0 here, so fall back to the visible
     * height rather than emitting 0, which no consumer can use. */
    uint32_t slice_h = (stride && sizeimg > stride) ? (sizeimg / stride) : 0;
    if (slice_h < h) slice_h = h;

    /* THE ESP32-P4's "YUV420" IS NOT I420.  esp_video maps V4L2_PIX_FMT_YUV420
     * onto CAM_CTLR_COLOR_YUV420 == ESP_COLOR_FOURCC_OUYY_EVYY: "Odd line UYY,
     * Even line VYY".  Every line is w/2 packed 3-byte groups (chroma, Y, Y),
     * so a line is w/2*3 bytes and the chroma type alternates per line — there
     * are no separate U/V planes at all.  Announcing it as YU12 made the host
     * decode a planar layout that does not exist, which is why a perfectly good
     * frame came out as noise.  Report the real fourcc and pitch instead; the
     * total size is identical (w*h*3/2), so nothing else changes. */
    if (fourcc == V4L2_PIX_FMT_YUV420) {
        fourcc  = v4l2_fourcc('O', 'U', 'Y', 'Y');
        stride  = w / 2 * 3;
        slice_h = h;
        NN_LOG_INF("P4 YUV420 is OUYY/EVYY line-packed — reporting fourcc=OUYY "
                   "stride=%u (not I420)", (unsigned)stride);
    }


    /* Borrow the next frame the capture loop dequeues.
     * TIMEOUT MUST EXCEED THE PRODUCER'S CADENCE: in DIAG builds the loop sleeps
     * CONFIG_NN_CAMERA_DIAG_INTERVAL_MS (default 5 s) between frames and also
     * spends seconds pushing ~7 MB over the WebSocket, so a 3 s wait expired
     * before a frame ever arrived.  30 s covers both; production (30 fps) hits
     * it in ~33 ms. */
    /* Pause the periodic hub push for the duration of the grab, then wait out
     * any push ALREADY in flight — a single 7.46 MB WebSocket write blocks the
     * capture loop for 10-20 s and cannot be interrupted, so the wait must
     * outlast it or `isp send` reports a timeout for a perfectly healthy loop. */
    const bool ws_was_on = nn_camera_diag_ws_enabled();
    nn_camera_diag_ws_enable(false);
    nn_camera__diag_arm();
    for (int i = 0; i < 2000 && !nn_camera__diag_len(); i++) {   /* 20 s: 4 diag frame periods */
        vTaskDelay(pdMS_TO_TICKS(10));
        if (i && i % 1000 == 0) NN_LOG_INF("waiting for a frame (%d s)", i / 100);
    }
    const uint32_t len = nn_camera__diag_len();
    if (!len) {
        nn_camera__diag_disarm();
        nn_camera_diag_ws_enable(ws_was_on);
        NN_LOG_ERR("no frame in 20 s (is the capture loop running?)");
        return ESP_ERR_TIMEOUT;
    }

    /* Does the payload match the geometry we are about to declare?  Compare
     * against the format's FULL frame size, not stride*height — planar YUV
     * carries chroma after the luma plane (I420 1920x1296 is 1944 rows' worth
     * of 1920-byte lines, which is correct, not a fault). */
    uint32_t bits = 0;
    switch (fourcc) {
    case V4L2_PIX_FMT_SBGGR8: case V4L2_PIX_FMT_GREY:    bits = 8;  break;
    case V4L2_PIX_FMT_YUV420:                            bits = 12; break;
    case V4L2_PIX_FMT_RGB565: case V4L2_PIX_FMT_YUV422P: bits = 16; break;
    case V4L2_PIX_FMT_RGB24:                             bits = 24; break;
    default: if (fourcc == v4l2_fourcc('O','U','Y','Y')) bits = 12; break;
    }
    if (bits) {
        const uint32_t expect = (uint32_t)((uint64_t)w * h * bits / 8);
        if (len != expect) {
            NN_LOG_WRN("payload %u B but %ux%u %.4s needs %u B — declared format "
                       "is NOT what the ISP emitted", (unsigned)len, (unsigned)w,
                       (unsigned)h, (const char *)&fourcc, (unsigned)expect);
        }
    }

    uint8_t hdr[NN_CAM_DIAG_HDR_SIZE];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, NN_CAM_DIAG_MAGIC, 4);              /* resync/sanity marker */
    strncpy((char *)hdr + 4, desc ? desc : "isp_out", 11);
    uint32_t *f = (uint32_t *)(hdr + 16);
    f[0] = w; f[1] = h; f[2] = fourcc; f[3] = stride; f[4] = slice_h; f[5] = len;

    /* send straight out of the borrowed frame, then release the capture loop */
    const esp_err_t r = nn_camera_diag_net_send(host, port, hdr, sizeof hdr,
                                                nn_camera__diag_buf(), len);
    nn_camera__diag_disarm();
    nn_camera_diag_ws_enable(ws_was_on);
    if (r != ESP_OK) return r;

    NN_LOG_INF("sent '%s' %ux%u fourcc=%.4s stride=%u slice=%u %u B -> %s:%u",
               desc ? desc : "isp_out", (unsigned)w, (unsigned)h,
               (const char *)&fourcc, (unsigned)stride, (unsigned)slice_h,
               (unsigned)len, host, (unsigned)port);
    return ESP_OK;
}
