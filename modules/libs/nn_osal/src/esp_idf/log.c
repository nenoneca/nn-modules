/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal log backend — ESP-IDF (esp_log). */
#include "nn_osal/log.h"
#include "esp_log.h"
#include <stdio.h>

static esp_log_level_t map_level(nn_osal_log_level_t l)
{
    switch (l) {
    case NN_OSAL_LOG_ERR: return ESP_LOG_ERROR;
    case NN_OSAL_LOG_WRN: return ESP_LOG_WARN;
    case NN_OSAL_LOG_INF: return ESP_LOG_INFO;
    case NN_OSAL_LOG_DBG: return ESP_LOG_DEBUG;
    default:              return ESP_LOG_NONE;
    }
}

void nn_osal_log_emit_v(nn_osal_log_level_t level, const char *module,
                        const char *fmt, va_list ap)
{
    esp_log_level_t el = map_level(level);
    const char *tag = module ? module : "nn";
    if (el == ESP_LOG_NONE || el > esp_log_level_get(tag))
        return;
    char line[256];
    vsnprintf(line, sizeof line, fmt, ap);
    static const char lc[] = { '-', 'E', 'W', 'I', 'D' };
    esp_log_write(el, tag, "%c (%lu) %s: %s\n",
                  lc[level <= NN_OSAL_LOG_DBG ? level : 0],
                  (unsigned long)esp_log_timestamp(), tag, line);
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
    vprintf(fmt, ap);
    va_end(ap);
}
