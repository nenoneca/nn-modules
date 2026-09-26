/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_pal/log_sink.h>

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <nn_osal/osal.h>

#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/logging/log_output.h>

NN_OSAL_LOG_MODULE(nn_pal_log_sink);

/*
 * Generic Zephyr log backend that forwards each processed message to a
 * caller-registered nn_pal_log_sink_cb_t.  Moves the previous
 * coap_log.c machinery into the PAL so lib code (node_mgr/coap_log.c)
 * doesn't include <zephyr/logging/log_backend.h> directly.
 */

#define OUT_BUF_SIZE   256

static nn_pal_log_sink_cb_t g_cb;
static void               *g_user;
static bool                g_active;
static bool                g_panic;
static uint8_t             g_cur_level;
static uint32_t            g_log_fmt = LOG_OUTPUT_TEXT;

static k_tid_t g_log_tid;

static int log_output_func(uint8_t *data, size_t length, void *ctx)
{
    (void)ctx;
    if (!g_active || g_panic || length == 0 || !g_cb) {
        return (int)length;
    }
    g_cb(g_cur_level, data, length, g_user);
    return (int)length;
}

static uint8_t g_out_buf[OUT_BUF_SIZE];
LOG_OUTPUT_DEFINE(g_log_output, log_output_func, g_out_buf, sizeof(g_out_buf));

static void backend_process(const struct log_backend *const backend,
                            union log_msg_generic *msg)
{
    (void)backend;
    if (!g_log_tid) g_log_tid = k_current_get();
    if (g_panic || !g_active || !g_cb) return;

    /* Capture the message's severity so the sink callback sees it. */
    g_cur_level = (uint8_t)log_msg_get_level(&msg->log);

    uint32_t flags = LOG_OUTPUT_FLAG_LEVEL | LOG_OUTPUT_FLAG_TIMESTAMP;
    log_format_func_t fn = log_format_func_t_get(g_log_fmt);
    fn(&g_log_output, &msg->log, flags);
}

static void backend_panic(const struct log_backend *const backend)
{
    (void)backend;
    g_panic = true;
}

static int backend_format_set(const struct log_backend *const backend,
                              uint32_t log_type)
{
    (void)backend;
    g_log_fmt = log_type;
    return 0;
}

static const struct log_backend_api g_api = {
    .process    = backend_process,
    .panic      = backend_panic,
    .format_set = backend_format_set,
};

LOG_BACKEND_DEFINE(nn_pal_log_sink, g_api, false);

int nn_pal_log_sink_register(nn_pal_log_sink_cb_t cb, void *user,
                             uint8_t min_level)
{
    if (!cb) return -EINVAL;
    g_cb     = cb;
    g_user   = user;
    g_active = true;
    log_backend_enable(&nn_pal_log_sink, NULL, min_level);
    NN_LOG_INF("log sink active at min_level=%u", min_level);
    return 0;
}

int nn_pal_log_sink_unregister(void)
{
    if (!g_active) return 0;
    log_backend_disable(&nn_pal_log_sink);
    g_active = false;
    g_cb     = NULL;
    g_user   = NULL;
    return 0;
}

void nn_pal_log_sink_panic(void)
{
    g_panic = true;
}

void nn_pal_log_thread_suspend(void)
{
    if (g_log_tid) k_thread_suspend(g_log_tid);
}

void nn_pal_log_thread_resume(void)
{
    if (g_log_tid) k_thread_resume(g_log_tid);
}
