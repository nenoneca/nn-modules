/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/*
 * nn_osal/shell.h — debug-shell command registration + I/O.
 *
 * Wraps the *registration* surface of Zephyr's shell subsystem.  The
 * shell itself stays Zephyr-native (parser, history, tab completion,
 * UART transport, etc.) — abstracting all of that is out of scope.
 *
 * What this gets you:
 *   - A single macro NN_OSAL_SHELL_CMD_REGISTER(name, handler, help)
 *     to publish a top-level command from any .c file.
 *   - Subcommand sets via NN_OSAL_SHELL_SUBCMD_SET_CREATE + ENTRY.
 *   - Output from handlers via nn_osal_shell_print / _error / _warn /
 *     _fprintf — calls take an opaque shell context (passed in to
 *     every handler).
 *
 * On a non-Zephyr backend, NN_OSAL_SHELL_CMD_REGISTER would expand to
 * an `__attribute__((constructor))` that calls the alternative shell
 * runtime's registration API; the output helpers would route to
 * stdout / a debug UART / wherever.
 */

/* Backend defines nn_osal_shell_ctx_t as a typedef alias for whatever
 * the underlying shell uses (on Zephyr: const struct shell).  This lets
 * the SHELL_CMD/CMD_ARG macros pass handlers through without trampolines
 * — handlers have exactly the backend's expected signature. */
#include "internal/backend_shell.h"

typedef int (*nn_osal_shell_cmd_t)(nn_osal_shell_ctx_t *sh,
                                   size_t argc, char **argv);

/* ── output helpers (use from inside a command handler) ──────────── */

void nn_osal_shell_print(nn_osal_shell_ctx_t *sh, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void nn_osal_shell_error(nn_osal_shell_ctx_t *sh, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void nn_osal_shell_warn(nn_osal_shell_ctx_t *sh, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void nn_osal_shell_print_v(nn_osal_shell_ctx_t *sh, const char *fmt,
                           va_list ap);

/* ── registration macros (compile-time) ────────────────────────── */

/* Register a top-level command:
 *   static int my_handler(nn_osal_shell_ctx_t *sh, size_t argc, char **argv) {
 *       nn_osal_shell_print(sh, "hello");
 *       return 0;
 *   }
 *   NN_OSAL_SHELL_CMD_REGISTER(my, my_handler, "my command");
 */
#define NN_OSAL_SHELL_CMD_REGISTER(name, handler, help)                     \
        _NN_OSAL_SHELL_CMD_REGISTER_IMPL(name, handler, help)

/* Register a subcommand set:
 *   static int do_foo(nn_osal_shell_ctx_t *sh, size_t argc, char **argv) {...}
 *   static int do_bar(nn_osal_shell_ctx_t *sh, size_t argc, char **argv) {...}
 *   NN_OSAL_SHELL_SUBCMD_SET_CREATE(my_subs,
 *       NN_OSAL_SHELL_CMD(foo, do_foo, "do foo"),
 *       NN_OSAL_SHELL_CMD(bar, do_bar, "do bar"),
 *   );
 *   NN_OSAL_SHELL_CMD_REGISTER_SET(my, &my_subs, NULL, "parent help");
 */
#define NN_OSAL_SHELL_SUBCMD_SET_CREATE(name, ...)                          \
        _NN_OSAL_SHELL_SUBCMD_SET_CREATE_IMPL(name, __VA_ARGS__)

/* Sentinel that closes a NN_OSAL_SHELL_SUBCMD_SET_CREATE entry list.
 * Place it as the final entry (with no trailing comma). */
#define NN_OSAL_SHELL_SUBCMD_SET_END  _NN_OSAL_SHELL_SUBCMD_SET_END_IMPL

#define NN_OSAL_SHELL_CMD(name, handler, help)                              \
        _NN_OSAL_SHELL_CMD_IMPL(name, handler, help)

/* Subcommand with mandatory + optional arg counts enforced by the parser.
 * `mandatory` includes the command name itself (so a command with no args
 * uses mandatory=1, optional=0). */
#define NN_OSAL_SHELL_CMD_ARG(name, handler, help, mandatory, optional)     \
        _NN_OSAL_SHELL_CMD_ARG_IMPL(name, handler, help, mandatory, optional)

#define NN_OSAL_SHELL_CMD_REGISTER_SET(name, subset, default_handler, help) \
        _NN_OSAL_SHELL_CMD_REGISTER_SET_IMPL(name, subset, default_handler, help)
