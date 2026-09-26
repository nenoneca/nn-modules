/* SPDX-License-Identifier: Apache-2.0 */
/* nn_infer PAL — BeagleY-AI: in-process C7x TIDL via onnxruntime's C API.
 *
 * Runs INSIDE the edgeai container (libonnxruntime.so 1.15.0 with the TIDL
 * execution provider).  dlopen'd so one binary still starts elsewhere.
 *
 * Contract copied from the validated npu_server.py (ONR-OD-8220 yolox-s):
 *   input  "images": uint8 NCHW {1,3,640,640}, BGR, resize_with_pad
 *          (corner-anchored, pad 114) — the CALLER prepares this per caps.
 *   output 0: float dets[1,N,5] = x1,y1,x2,y2,score   (640-space)
 *   output 1: int64/float labels[1,N]
 * Model dir: <model_dir>/model/*.onnx, artifacts: <model_dir>/artifacts. */
#include <nn_infer/nn_infer.h>
#include <nn_osal/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>

NN_OSAL_LOG_MODULE(nn_infer);

/* minimal ORT C API surface (mirrors onnxruntime_c_api.h, version 15) */
#define ORT_API_VERSION_USED 15
typedef struct OrtApiBase OrtApiBase;
typedef struct OrtApi OrtApi;
typedef struct OrtEnv OrtEnv;
typedef struct OrtStatus OrtStatus;
typedef struct OrtSession OrtSession;
typedef struct OrtSessionOptions OrtSessionOptions;
typedef struct OrtMemoryInfo OrtMemoryInfo;
typedef struct OrtValue OrtValue;
typedef struct OrtRunOptions OrtRunOptions;

/* The OrtApi is a struct of function pointers with a FIXED layout.  We can't
 * replicate 300 members portably — instead we include the real header at
 * BUILD time inside the container.  This file therefore expects
 * onnxruntime_c_api.h to be available (container: /usr/include/onnxruntime/
 * include/onnxruntime/core/session).  Only dlopen of the LIBRARY is dynamic. */
#include <onnxruntime_c_api.h>

typedef struct {              /* tidl_provider_factory.h (TI 1.15 fork) */
    int   debug_level;
    char  artifacts_folder[512];
    int   priority;
    float max_pre_empt_delay;
    int   core_number;
} c_api_tidl_options;

typedef OrtStatus *(*set_default_tidl_fn)(c_api_tidl_options *);
typedef OrtStatus *(*append_tidl_fn)(OrtSessionOptions *, c_api_tidl_options *);

static void            *s_lib;
static const OrtApi    *s_api;
static OrtEnv          *s_env;
static OrtSession      *s_sess;
static OrtMemoryInfo   *s_meminfo;
static char             s_input_name[64] = "images";
static char             s_out_name[2][64] = { "dets", "labels" };

static nn_infer_caps_t s_caps = {
    .width = 640, .height = 640, .format = NN_INFER_FMT_BGR888,
    .fps = 5, .nclasses = 80, .model = "yolox_s_tidl",
};
/* Channels the loaded model actually wants -- read from the session, not
 * assumed.  A 1-channel (luma) model skips the HWC->CHW deinterleave below,
 * which is a 1.2 MB strided scatter per inference. */
static int s_in_ch = 3;

#define ORT_OK(expr) do { OrtStatus *_st = (expr); if (_st) { \
    NN_LOG_ERR("ort: %s", s_api->GetErrorMessage(_st)); \
    s_api->ReleaseStatus(_st); return -EIO; } } while (0)

int nn_infer_init(const char *model_dir)
{
    if (!model_dir) return -EINVAL;
    s_lib = dlopen("libonnxruntime.so", RTLD_NOW | RTLD_GLOBAL);
    if (!s_lib) { NN_LOG_ERR("libonnxruntime: %s", dlerror()); return -ENOENT; }
    const OrtApiBase *(*get_base)(void) = dlsym(s_lib, "OrtGetApiBase");
    set_default_tidl_fn set_def = dlsym(s_lib, "OrtSessionsOptionsSetDefault_Tidl");
    append_tidl_fn append_tidl = dlsym(s_lib, "OrtSessionOptionsAppendExecutionProvider_Tidl");
    if (!get_base) return -ENOSYS;
    s_api = get_base()->GetApi(ORT_API_VERSION_USED);
    if (!s_api) { NN_LOG_ERR("ORT api v%d unavailable", ORT_API_VERSION_USED); return -ENOSYS; }

    ORT_OK(s_api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "nn_infer", &s_env));
    OrtSessionOptions *so = NULL;
    ORT_OK(s_api->CreateSessionOptions(&so));

    char path[600];
    if (set_def && append_tidl) {
        static c_api_tidl_options topt;
        OrtStatus *st = set_def(&topt);
        if (st) { s_api->ReleaseStatus(st); }
        snprintf(topt.artifacts_folder, sizeof topt.artifacts_folder,
                 "%s/artifacts", model_dir);
        /* NN_INFER_TIDL_DEBUG makes TIDL report what it actually took.
         *
         * "TIDL EP attached" below means only that appending the provider
         * returned OK -- it says NOTHING about whether any node was claimed.
         * ONNX Runtime silently runs on the ARM CPU whatever TIDL declines,
         * so with debug_level 0 a model executing entirely on the A53 logs
         * exactly the same line as one fully offloaded to the C7x.  Set this
         * to 1 (or 2) and TIDL prints its subgraph summary -- "Number of
         * subgraphs", offloaded vs total nodes -- which is the only direct
         * evidence of where the model runs. */
        const char *dbg = getenv("NN_INFER_TIDL_DEBUG");
        topt.debug_level = dbg ? atoi(dbg) : 0;
        st = append_tidl(so, &topt);
        if (st) {
            NN_LOG_WRN("TIDL EP attach failed (%s) — CPU fallback",
                       s_api->GetErrorMessage(st));
            s_api->ReleaseStatus(st);
        } else {
            NN_LOG_INF("TIDL EP attached (%s/artifacts), debug_level=%d "
                       "(attach only; set NN_INFER_TIDL_DEBUG=1 to see what "
                       "it actually offloads)", model_dir, topt.debug_level);
        }
    } else {
        NN_LOG_WRN("TIDL EP symbols missing — CPU fallback");
    }

    /* The model is the user's to swap, so find the .onnx by SCANNING
     * <model_dir>/model/ -- never by name.  The fallback here used to be the
     * literal "yolox_s_lite_640x640_20220221_model.onnx", which meant any
     * model not named exactly that failed with "File doesn't exist" even
     * though it was installed correctly and complete. */
    char onnx[512] = "";
    snprintf(path, sizeof path, "%s/model", model_dir);
    DIR *dp = opendir(path);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp))) {
            size_t l = strlen(de->d_name);
            if (l > 5 && !strcmp(de->d_name + l - 5, ".onnx")) {
                snprintf(onnx, sizeof onnx, "%s/model/%s", model_dir, de->d_name);
                break;
            }
        }
        closedir(dp);
    }
    if (!onnx[0]) {
        NN_LOG_ERR("no .onnx under %s/model", model_dir);
        s_api->ReleaseSessionOptions(so);
        return -ENOENT;
    }
    snprintf(path, sizeof path, "%s", onnx);
    ORT_OK(s_api->CreateSession(s_env, path, so, &s_sess));
    s_api->ReleaseSessionOptions(so);
    ORT_OK(s_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                      &s_meminfo));

    /* discover the model's REAL tensor names (TI exports vary) */
    OrtAllocator *alloc = NULL;
    ORT_OK(s_api->GetAllocatorWithDefaultOptions(&alloc));
    char *nm = NULL;
    ORT_OK(s_api->SessionGetInputName(s_sess, 0, alloc, &nm));
    snprintf(s_input_name, sizeof s_input_name, "%s", nm);
    alloc->Free(alloc, nm);

    /* Read the model's real input geometry instead of assuming 3x640x640.
     * The channel count decides whether a frame needs interleaving at all,
     * and the app needs the format to know what to hand us. */
    OrtTypeInfo *ti0 = NULL;
    if (s_api->SessionGetInputTypeInfo(s_sess, 0, &ti0) == NULL && ti0) {
        const OrtTensorTypeAndShapeInfo *si = NULL;
        if (s_api->CastTypeInfoToTensorInfo(ti0, &si) == NULL && si) {
            size_t nd = 0;
            int64_t d[4] = { 0, 0, 0, 0 };
            s_api->GetDimensionsCount((OrtTensorTypeAndShapeInfo *)si, &nd);
            s_api->GetDimensions((OrtTensorTypeAndShapeInfo *)si, d, nd < 4 ? nd : 4);
            if (nd == 4 && d[1] > 0 && d[2] > 0 && d[3] > 0) {
                s_in_ch        = (int)d[1];
                s_caps.height  = (uint16_t)d[2];
                s_caps.width   = (uint16_t)d[3];
                s_caps.format  = (s_in_ch == 1) ? NN_INFER_FMT_GRAY8
                                                : NN_INFER_FMT_BGR888;
                snprintf(s_caps.model, sizeof s_caps.model, "%s",
                         s_in_ch == 1 ? "yolox_s_tidl_gray" : "yolox_s_tidl");
            }
        }
        s_api->ReleaseTypeInfo(ti0);
    }
    NN_LOG_INF("model input: %dx%d x%d ch -> %s", s_caps.width, s_caps.height,
               s_in_ch, s_in_ch == 1 ? "GRAY8 (no interleave, no colour convert)"
                                     : "BGR888 (HWC->CHW each frame)");
    size_t nout = 0;
    ORT_OK(s_api->SessionGetOutputCount(s_sess, &nout));
    for (size_t i = 0; i < 2 && i < nout; i++) {
        ORT_OK(s_api->SessionGetOutputName(s_sess, i, alloc, &nm));
        snprintf(s_out_name[i], sizeof s_out_name[i], "%s", nm);
        alloc->Free(alloc, nm);
    }
    NN_LOG_INF("ORT session up: %s in=%s out=%s,%s (%zu outs)",
               path, s_input_name, s_out_name[0], s_out_name[1], nout);
    return 0;
}

void nn_infer_deinit(void)
{
    if (s_api) {
        if (s_sess)    s_api->ReleaseSession(s_sess);
        if (s_meminfo) s_api->ReleaseMemoryInfo(s_meminfo);
        if (s_env)     s_api->ReleaseEnv(s_env);
    }
    if (s_lib) dlclose(s_lib);
    s_sess = NULL; s_meminfo = NULL; s_env = NULL; s_lib = NULL; s_api = NULL;
}

int nn_infer_query_caps(nn_infer_caps_t *caps)
{
    if (!caps) return -EINVAL;
    *caps = s_caps;
    return 0;
}

int nn_infer_run(const nn_infer_input_t *in, nn_infer_result_t *out)
{
    if (!in || !out || !s_sess) return -EINVAL;
    void *px = NULL;
    int rc = nn_osal_buf_map(&in->buf, &px);
    if (rc) return rc;

    const int W = s_caps.width, H = s_caps.height;
    static uint8_t chw[3 * 640 * 640];
    const uint8_t *hwc = px;
    const uint8_t *tensor;
    size_t tensor_sz;

    if (s_in_ch == 1) {
        /* A single plane is ALREADY NCHW -- hand the caller's buffer straight
         * to ORT.  Nothing is read, written or rearranged here. */
        tensor = hwc;
        tensor_sz = (size_t)W * H;
    } else {
        /* Interleaved BGR888 HWC -> planar NCHW.  This is a 1.2 MB strided
         * scatter on every inference and it lives INSIDE nn_infer_run, which
         * is why it was long mistaken for cost inside the TIDL EP. */
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < W * H; i++)
                chw[c * W * H + i] = hwc[i * 3 + c];
        tensor = chw;
        tensor_sz = sizeof chw;
    }

    int64_t shape[4] = { 1, s_in_ch, H, W };
    OrtValue *inv = NULL;
    ORT_OK(s_api->CreateTensorWithDataAsOrtValue(s_meminfo, (void *)tensor,
        tensor_sz, shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8, &inv));

    const char *in_names[]  = { s_input_name };
    const char *out_names[] = { s_out_name[0], s_out_name[1] };
    OrtValue *outs[2] = { NULL, NULL };
    OrtStatus *st = s_api->Run(s_sess, NULL, in_names,
                               (const OrtValue *const *)&inv, 1,
                               out_names, 2, outs);
    s_api->ReleaseValue(inv);
    nn_osal_buf_unmap(&in->buf, px);   /* after Run: the 1-ch path wraps it */
    if (st) {
        NN_LOG_WRN("ort run: %s", s_api->GetErrorMessage(st));
        s_api->ReleaseStatus(st);
        return -EIO;
    }

    float   *dets = NULL;
    void    *labels = NULL;
    ORT_OK(s_api->GetTensorMutableData(outs[0], (void **)&dets));
    ORT_OK(s_api->GetTensorMutableData(outs[1], &labels));

    /* count from output 0 shape: [1, N, 5] */
    OrtTensorTypeAndShapeInfo *ti = NULL;
    ORT_OK(s_api->GetTensorTypeAndShape(outs[0], &ti));
    int64_t dims[3] = {0}; size_t nd = 0;
    ORT_OK(s_api->GetDimensionsCount(ti, &nd));
    ORT_OK(s_api->GetDimensions(ti, dims, nd < 3 ? nd : 3));
    ONNXTensorElementDataType lt;
    OrtTensorTypeAndShapeInfo *lti = NULL;
    ORT_OK(s_api->GetTensorTypeAndShape(outs[1], &lti));
    ORT_OK(s_api->GetTensorElementType(lti, &lt));
    int n = (int)(nd >= 2 ? dims[nd - 2] : 0);
    if (n > NN_INFER_MAX_DET) n = NN_INFER_MAX_DET;

    out->ts_ms = in->ts_ms;
    out->count = 0;
    for (int i = 0; i < n; i++) {
        float x1 = dets[i * 5], y1 = dets[i * 5 + 1];
        float x2 = dets[i * 5 + 2], y2 = dets[i * 5 + 3], sc = dets[i * 5 + 4];
        if (sc <= 0.05f) continue;
        long cls = (lt == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
                   ? (long)((int64_t *)labels)[i]
                   : (long)((float *)labels)[i];
        nn_infer_det_t *d = &out->det[out->count++];
        d->x = (uint16_t)(x1 < 0 ? 0 : x1);
        d->y = (uint16_t)(y1 < 0 ? 0 : y1);
        d->w = (uint16_t)(x2 > x1 ? x2 - x1 : 0);
        d->h = (uint16_t)(y2 > y1 ? y2 - y1 : 0);
        d->class_id = (uint16_t)cls;
        d->conf_x1000 = (uint16_t)(sc * 1000.f);
    }
    s_api->ReleaseTensorTypeAndShapeInfo(ti);
    s_api->ReleaseTensorTypeAndShapeInfo(lti);
    s_api->ReleaseValue(outs[0]);
    s_api->ReleaseValue(outs[1]);
    return 0;
}
