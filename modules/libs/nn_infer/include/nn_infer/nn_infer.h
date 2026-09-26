/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <nn_osal/buf.h>

/* nn_infer — edge-side object detection behind a portable API.
 * Core is platform-free; a PAL (src/pal/<name>/) does the actual inference:
 *   byai_dlr — BeagleY-AI C7x TIDL, in-process via neo-ai-dlr (libdlr)
 *   stub     — fixed fake detections (CI / protocol tests)
 * See DESIGN.md for the wire format ('D' 0x44 record) and service policy. */

typedef enum {
    NN_INFER_FMT_RGB888 = 0,
    NN_INFER_FMT_BGR888 = 1,
    NN_INFER_FMT_NV12   = 2,
    /* Single 8-bit luma plane.  A 1-channel model needs no interleave and no
     * colour convert: the ISP's Y plane IS the tensor, so the whole
     * HWC->CHW/BGR path collapses to a plane copy. */
    NN_INFER_FMT_GRAY8  = 3,
} nn_infer_fmt_t;

/* What the PAL WANTS as input — the app resamples camera output to match. */
typedef struct {
    uint16_t        width;
    uint16_t        height;
    nn_infer_fmt_t  format;
    uint8_t         fps;         /* desired inference rate */
    uint16_t        nclasses;
    char            model[32];   /* short id, e.g. "yolox_s_tidl" */
} nn_infer_caps_t;

typedef struct {
    nn_osal_buf_t buf;           /* frame in caps format (black box handle) */
    uint64_t      ts_ms;         /* capture timestamp */
} nn_infer_input_t;

typedef struct {
    uint16_t x, y, w, h;         /* pixels, in the INPUT's coordinate space */
    uint16_t class_id;
    uint16_t conf_x1000;         /* confidence * 1000 */
} nn_infer_det_t;

#define NN_INFER_MAX_DET 32

typedef struct {
    uint64_t       ts_ms;        /* echoed from the input */
    uint8_t        count;
    nn_infer_det_t det[NN_INFER_MAX_DET];
} nn_infer_result_t;

/* model_dir: PAL-specific model location (byai_dlr: compiled TIDL artifact
 * directory). Returns 0 or -errno. */
int  nn_infer_init(const char *model_dir);
int  nn_infer_query_caps(nn_infer_caps_t *caps);
/* Synchronous — run on a dedicated thread; NOT the capture callback. */
int  nn_infer_run(const nn_infer_input_t *in, nn_infer_result_t *out);
void nn_infer_deinit(void);
