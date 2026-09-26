/* SPDX-License-Identifier: Apache-2.0 */
/* nn_infer PAL — BeagleY-AI: in-process TIDL (C7x) via neo-ai-dlr's C API.
 *
 * Runs INSIDE the edgeai container (TI userspace) — the camera app lives
 * there anyway for the tiovx pipeline.  libdlr.so is dlopen'd so one binary
 * still starts on hosts without it (init fails cleanly instead of at load).
 *
 * Model: a DLR/TIDL-compiled YOLOX artifact directory (the same artifacts
 * npu_server.py served).  Output convention follows TI's compiled YOLOX:
 * dets[N][5] = x1,y1,x2,y2,score (input letterbox space) + labels[N].
 * VERIFY against the actual artifact on first on-target run — adjust
 * out_layout if the artifact differs. */
#include <nn_infer/nn_infer.h>
#include <nn_osal/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

NN_OSAL_LOG_MODULE(nn_infer);

typedef void *DLRModelHandle;
static int   (*p_CreateDLRModel)(DLRModelHandle *, const char *, int, int);
static int   (*p_DeleteDLRModel)(DLRModelHandle *);
static int   (*p_SetDLRInput)(DLRModelHandle *, const char *, const int64_t *, const void *, int);
static int   (*p_RunDLRModel)(DLRModelHandle *);
static int   (*p_GetDLROutput)(DLRModelHandle *, int, void *);
static int   (*p_GetDLROutputShape)(DLRModelHandle *, int, int64_t *);
static int   (*p_GetDLRNumOutputs)(DLRModelHandle *, int *);
static const char *(*p_DLRGetLastError)(void);

static void          *s_lib;
static DLRModelHandle s_model;
static nn_infer_caps_t s_caps = {
    .width = 640, .height = 640, .format = NN_INFER_FMT_RGB888,
    .fps = 5, .nclasses = 80, .model = "yolox_s_tidl",
};

#define RESOLVE(sym) do { p_##sym = dlsym(s_lib, #sym); \
    if (!p_##sym) { NN_LOG_ERR("libdlr: missing %s", #sym); return -ENOSYS; } } while (0)

int nn_infer_init(const char *model_dir)
{
    if (!model_dir) return -EINVAL;
    s_lib = dlopen("libdlr.so", RTLD_NOW);
    if (!s_lib)  /* python wheel install path inside the TI SDK rootfs */
        s_lib = dlopen("/usr/lib/python3.12/site-packages/dlr/libdlr.so", RTLD_NOW);
    if (!s_lib) { NN_LOG_ERR("libdlr.so not found: %s", dlerror()); return -ENOENT; }
    RESOLVE(CreateDLRModel); RESOLVE(DeleteDLRModel); RESOLVE(SetDLRInput);
    RESOLVE(RunDLRModel);    RESOLVE(GetDLROutput);   RESOLVE(GetDLROutputShape);
    RESOLVE(GetDLRNumOutputs); RESOLVE(DLRGetLastError);
    /* dev_type 1 = CPU id per DLR convention; TIDL dispatch is baked into
     * the compiled artifact itself. */
    if (p_CreateDLRModel(&s_model, model_dir, 1, 0) != 0) {
        NN_LOG_ERR("CreateDLRModel(%s): %s", model_dir, p_DLRGetLastError());
        return -EIO;
    }
    NN_LOG_INF("TIDL model up: %s", model_dir);
    return 0;
}

void nn_infer_deinit(void)
{
    if (s_model) p_DeleteDLRModel(&s_model);
    if (s_lib) dlclose(s_lib);
    s_model = NULL; s_lib = NULL;
}

int nn_infer_query_caps(nn_infer_caps_t *caps)
{
    if (!caps) return -EINVAL;
    *caps = s_caps;
    return 0;
}

int nn_infer_run(const nn_infer_input_t *in, nn_infer_result_t *out)
{
    if (!in || !out || !s_model) return -EINVAL;
    void *px = NULL;
    int rc = nn_osal_buf_map(&in->buf, &px);
    if (rc) return rc;

    /* TI-compiled YOLOX takes NCHW-agnostic uint8 RGB via the "images"
     * input; shape {1,H,W,3}. */
    int64_t shape[4] = { 1, s_caps.height, s_caps.width, 3 };
    rc = p_SetDLRInput(&s_model, "images", shape, px, 4);
    nn_osal_buf_unmap(&in->buf, px);
    if (rc) { NN_LOG_WRN("SetDLRInput: %s", p_DLRGetLastError()); return -EIO; }
    if (p_RunDLRModel(&s_model)) {
        NN_LOG_WRN("RunDLRModel: %s", p_DLRGetLastError());
        return -EIO;
    }

    /* out0: float dets[N][5] (x1,y1,x2,y2,score), out1: float/int labels[N] */
    int64_t sh[4] = {0};
    if (p_GetDLROutputShape(&s_model, 0, sh)) return -EIO;
    int n = (int)sh[0];
    if (n > NN_INFER_MAX_DET) n = NN_INFER_MAX_DET;
    static float  dets[NN_INFER_MAX_DET * 8];
    static float  labels[NN_INFER_MAX_DET * 2];
    if (p_GetDLROutput(&s_model, 0, dets))   return -EIO;
    if (p_GetDLROutput(&s_model, 1, labels)) return -EIO;

    out->ts_ms = in->ts_ms;
    out->count = 0;
    for (int i = 0; i < n; i++) {
        float x1 = dets[i * 5 + 0], y1 = dets[i * 5 + 1];
        float x2 = dets[i * 5 + 2], y2 = dets[i * 5 + 3];
        float sc = dets[i * 5 + 4];
        if (sc <= 0.f) continue;
        nn_infer_det_t *d = &out->det[out->count++];
        d->x = (uint16_t)(x1 < 0 ? 0 : x1);
        d->y = (uint16_t)(y1 < 0 ? 0 : y1);
        d->w = (uint16_t)(x2 > x1 ? x2 - x1 : 0);
        d->h = (uint16_t)(y2 > y1 ? y2 - y1 : 0);
        d->class_id   = (uint16_t)labels[i];
        d->conf_x1000 = (uint16_t)(sc * 1000.f);
    }
    return 0;
}
