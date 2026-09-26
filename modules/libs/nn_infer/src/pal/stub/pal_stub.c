/* SPDX-License-Identifier: Apache-2.0 */
/* nn_infer PAL — stub: one fixed detection, for CI and protocol tests. */
#include <nn_infer/nn_infer.h>
#include <string.h>

int nn_infer_init(const char *model_dir) { (void)model_dir; return 0; }
void nn_infer_deinit(void) {}

int nn_infer_query_caps(nn_infer_caps_t *caps)
{
    if (!caps) return -1;
    memset(caps, 0, sizeof *caps);
    caps->width = 640; caps->height = 640;
    caps->format = NN_INFER_FMT_RGB888;
    caps->fps = 5; caps->nclasses = 80;
    strcpy(caps->model, "stub");
    return 0;
}

int nn_infer_run(const nn_infer_input_t *in, nn_infer_result_t *out)
{
    if (!in || !out) return -1;
    out->ts_ms = in->ts_ms;
    out->count = 1;
    out->det[0] = (nn_infer_det_t){ .x = 160, .y = 160, .w = 320, .h = 320,
                                    .class_id = 0, .conf_x1000 = 900 };
    return 0;
}
