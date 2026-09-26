/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

/* Most call sites use the NN_LOG_* macros which expand to LOG_INF /
 * LOG_WRN / etc. directly (no function-call indirection).  The
 * emit_v / emit / printk functions below cover the runtime path used
 * by panic + pre-init code where the macros aren't applicable. */

LOG_MODULE_REGISTER(nn_osal, LOG_LEVEL_INF);

void nn_osal_log_emit_v(nn_osal_log_level_t lvl, const char *module,
                        const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)(sizeof buf - 1);
    buf[n] = '\0';
    switch (lvl) {
    case NN_OSAL_LOG_ERR: LOG_ERR("[%s] %s", module ? module : "?", buf); break;
    case NN_OSAL_LOG_WRN: LOG_WRN("[%s] %s", module ? module : "?", buf); break;
    case NN_OSAL_LOG_INF: LOG_INF("[%s] %s", module ? module : "?", buf); break;
    case NN_OSAL_LOG_DBG: LOG_DBG("[%s] %s", module ? module : "?", buf); break;
    default: break;
    }
}

void nn_osal_log_emit(nn_osal_log_level_t lvl, const char *module,
                      const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    nn_osal_log_emit_v(lvl, module, fmt, ap);
    va_end(ap);
}

void nn_osal_printk(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
}
