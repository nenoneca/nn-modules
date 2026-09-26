/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/*
 * fw_common/log.h — minimal cross-platform logging shim.
 *
 * Under Zephyr: forwards everything to the real Zephyr LOG subsystem.
 * Under plain Linux (or any non-Zephyr host): expands to stderr fprintf
 * with a one-letter severity prefix.
 *
 * Each .c file should call LOG_MODULE_REGISTER(<name>, <level>) exactly
 * once at file scope, then use LOG_{ERR,WRN,INF,DBG}() like usual.  On
 * Linux, LOG_MODULE_REGISTER is a no-op, and the module name is baked
 * into the log line via a per-file static.
 */

#ifdef __ZEPHYR__

#include <zephyr/logging/log.h>

#else /* !__ZEPHYR__ — POSIX / Linux host build */

#include <stdio.h>
#include <stdarg.h>

/* Zephyr's log levels — keep numbers compatible. */
#define LOG_LEVEL_NONE 0
#define LOG_LEVEL_ERR  1
#define LOG_LEVEL_WRN  2
#define LOG_LEVEL_INF  3
#define LOG_LEVEL_DBG  4

#ifndef FW_COMMON_LOG_LEVEL_DEFAULT
#define FW_COMMON_LOG_LEVEL_DEFAULT LOG_LEVEL_INF
#endif

/* Per-translation-unit logger handle.  LOG_MODULE_REGISTER stores the
 * module name in a file-static so LOG_<LEVEL> can prepend it.  The
 * optional second arg (a LOG_LEVEL_*) is accepted for source compat
 * but ignored — host build always uses FW_COMMON_LOG_LEVEL_DEFAULT. */
#define LOG_MODULE_REGISTER(_name, ...)					\
	static const char fw_log_module_name[] = #_name

#define LOG_MODULE_DECLARE(_name, ...)					\
	extern const char fw_log_module_name[]

#define FW_LOG_PRINT(_sev_char, _level, _fmt, ...)			\
	do {								\
		if ((_level) <= FW_COMMON_LOG_LEVEL_DEFAULT) {		\
			fprintf(stderr, "[%c %s] " _fmt "\n",		\
				(_sev_char), fw_log_module_name,	\
				##__VA_ARGS__);				\
		}							\
	} while (0)

#define LOG_ERR(fmt, ...)   FW_LOG_PRINT('E', LOG_LEVEL_ERR, fmt, ##__VA_ARGS__)
#define LOG_WRN(fmt, ...)   FW_LOG_PRINT('W', LOG_LEVEL_WRN, fmt, ##__VA_ARGS__)
#define LOG_INF(fmt, ...)   FW_LOG_PRINT('I', LOG_LEVEL_INF, fmt, ##__VA_ARGS__)
#define LOG_DBG(fmt, ...)   FW_LOG_PRINT('D', LOG_LEVEL_DBG, fmt, ##__VA_ARGS__)

/* Hexdump becomes a single-line "msg: <N bytes>"; refine later if useful. */
#define LOG_HEXDUMP_INF(_buf, _n, _msg)                                  \
	LOG_INF("%s: %zu B", (_msg), (size_t)(_n))
#define LOG_HEXDUMP_DBG(_buf, _n, _msg)                                  \
	LOG_DBG("%s: %zu B", (_msg), (size_t)(_n))

#endif /* __ZEPHYR__ */
