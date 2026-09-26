/* SPDX-License-Identifier: Apache-2.0 */

/*
 * coap_log.c — log sink that forwards log lines to the hub over the
 * nn_proto control plane (D2H LOG_LINE, fire-and-forget).
 *
 * Originally a Zephyr LOG_BACKEND_DEFINE that fed log_output directly;
 * now uses nn_pal/log_sink.h so the Zephyr-specific bits live in the
 * PAL backend.  Public API name kept (`coap_log_*`) so existing call
 * sites compile unchanged — rename in a follow-up sweep.
 */

#include <nn_osal/osal.h>
#include <node_mgr/coap_log.h>
#include <node_mgr/nn_proto_client.h>
#include <nn_pal/log_sink.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <nn_proto/nn_proto.h>

NN_OSAL_LOG_MODULE(coap_log);

#define LOG_BUF_SIZE   256

static bool g_enabled;

/* PAL → D2H bridge.  One formatted log line per call; fire-and-forget. */
static void log_sink_cb(uint8_t level, const uint8_t *data, size_t len,
			void *user)
{
	ARG_UNUSED(level); ARG_UNUSED(user);
	if (!g_enabled || len == 0) return;
	(void)nn_proto_client_send_d2h_cmd(NN_PROTO_CMD_LOG_LINE, data, len);
}

void coap_log_thread_suspend(void) { nn_pal_log_thread_suspend(); }
void coap_log_thread_resume(void)  { nn_pal_log_thread_resume();  }

int coap_log_start(const char *unused)
{
	ARG_UNUSED(unused);
	if (g_enabled) return 0;
	g_enabled = true;
	/* Cap mesh-side filter at CONFIG_NODE_MGR_COAP_LOG_LEVEL (default
	 * WRN=2 — this comment claimed WRN while the Kconfig default was
	 * actually INF, which is how the hot-path INF lines shipped
	 * unnoticed).  The UART backend still gets every level it is
	 * configured for; only what crosses the radio is throttled. */
	return nn_pal_log_sink_register(log_sink_cb, NULL,
					CONFIG_NODE_MGR_COAP_LOG_LEVEL);
}

void coap_log_stop(void)
{
	if (!g_enabled) return;
	nn_pal_log_sink_unregister();
	g_enabled = false;
}

void coap_log_send(const char *fmt, ...)
{
	if (!g_enabled) return;
	char text[LOG_BUF_SIZE];
	va_list ap;
	va_start(ap, fmt);
	int len = vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	if (len <= 0) return;
	if (len >= (int)sizeof text) len = sizeof text - 1;
	log_sink_cb(0, (const uint8_t *)text, (size_t)len, NULL);
}

void coap_log_flush(void) { /* sends are synchronous from the log thread */ }
