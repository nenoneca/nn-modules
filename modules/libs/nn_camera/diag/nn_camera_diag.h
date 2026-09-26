/* nn_camera diagnostics — pipeline observation, separate from the streaming path.
 *
 * The P4 ISP is fixed-function with ONE DMA output, so the pipeline can only be
 * observed at its tail.  Which DOMAIN that tail sits in is chosen by the capture
 * pixel format (esp_video keeps the ISP in-path for SBGGR8 / RGB565 / RGB24 /
 * YUV420 / YUV422P):
 *
 *   SBGGR8 -> Bayer domain   : before demosaic / WBG / CCM / gamma / sharpen
 *   RGB24  -> RGB domain     : after the full RGB chain, before RGB2YUV
 *   YUV420 -> production tail: everything applied
 *
 * Per-STAGE contribution is obtained by DIFFERENCING captures taken with
 * individual blocks disabled (`isp blk <name> 0`) — not by tapping mid-chain,
 * which the hardware does not permit.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── WebSocket diag sink (legacy /diag/isp path on the media service) ─────── */
void *nn_cam_diag_open(const char *uri);
bool  nn_cam_diag_connected(void *h);
int   nn_cam_diag_text(void *h, const char *s, int len, int to_ms);
int   nn_cam_diag_bin(void *h, const void *d, int len, int to_ms);
void  nn_cam_diag_close(void *h);

/* ── TCP frame tap (media/tools/diag_recv.py on the dev host) ─────────────── */

/* 40-byte header preceding every frame, little-endian:
 *   [4B magic "NNDG"][12B description][u32 width][u32 height][u32 fourcc]
 *   [u32 stride bytes/row][u32 slice_height][u32 data_size]
 * The magic lets a receiver resynchronise instead of trusting stream position,
 * and lets it reject a mis-framed connection immediately rather than allocating
 * against a garbage length.
 * stride and slice_height are taken from the driver's own G_FMT, so padding is
 * reported rather than assumed — a consumer that ignores them skews rows or
 * reads chroma planes from the wrong offset. */
#define NN_CAM_DIAG_MAGIC "NNDG"
#define NN_CAM_DIAG_HDR_SIZE 40

/* Raw socket send; hdr/data are written back-to-back on one connection. */
esp_err_t nn_camera_diag_net_send(const char *host, uint16_t port,
                                  const void *hdr, size_t hdr_len,
                                  const void *data, size_t data_len);

/* Grab the next capture-device frame and push it to host:port with the header
 * above.  Console: `isp send <host> [port] [desc]`. */
/* Enable/disable the PERIODIC WebSocket push to the hub.  Turn it off before
 * driving on-demand captures: the two share one capture loop and a full-frame
 * push blocks it for 10-20 s. */
void      nn_camera_diag_ws_enable(bool on);
bool      nn_camera_diag_ws_enabled(void);

/* MIPI-CSI2 link error statistics (ECC / CRC / frame-sync / lane-sync).
 * Unmasks the CSI host interrupt-status registers on first use; the bits are
 * read-to-clear so each dump covers the interval since the previous one. */
void      nn_camera_diag_csi_dump(void);
void      nn_camera_diag_csi_reset(void);

esp_err_t nn_camera_diag_send(const char *host, uint16_t port, const char *desc);

#ifdef __cplusplus
}
#endif
