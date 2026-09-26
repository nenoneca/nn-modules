/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal log backend — POSIX (stderr, journald-friendly single lines).
 * Runtime level via NN_OSAL_LOG_LEVEL env: err|wrn|inf|dbg (default inf). */
#include "nn_osal/log.h"
#include "nn_osal/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nn_osal_log_level_t runtime_level(void)
{
    static int lvl = -1;
    if (lvl < 0) {
        const char *e = getenv("NN_OSAL_LOG_LEVEL");
        lvl = NN_OSAL_LOG_INF;
        if (e) {
            if      (!strcmp(e, "err")) lvl = NN_OSAL_LOG_ERR;
            else if (!strcmp(e, "wrn")) lvl = NN_OSAL_LOG_WRN;
            else if (!strcmp(e, "dbg")) lvl = NN_OSAL_LOG_DBG;
        }
    }
    return (nn_osal_log_level_t)lvl;
}

void nn_osal_log_emit_v(nn_osal_log_level_t level, const char *module,
                        const char *fmt, va_list ap)
{
    if (level > runtime_level()) return;
    static const char lc[] = { '-', 'E', 'W', 'I', 'D' };
    char line[256];
    vsnprintf(line, sizeof line, fmt, ap);
    fprintf(stderr, "%c (%lld) %s: %s\n",
            lc[level <= NN_OSAL_LOG_DBG ? level : 0],
            (long long)nn_osal_uptime_ms(), module ? module : "nn", line);
}

void nn_osal_log_emit(nn_osal_log_level_t level, const char *module,
                      const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nn_osal_log_emit_v(level, module, fmt, ap);
    va_end(ap);
}

void nn_osal_printk(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
