/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#ifdef CONFIG_NN_OSAL_BACKEND_ZEPHYR
#  include <zephyr/shell/shell.h>

/* On the Zephyr backend, `nn_osal_shell_ctx_t` is just an alias for
 * `const struct shell`.  Handler functions therefore have a signature
 * that's bit-for-bit identical to Zephyr's `shell_cmd_handler`, and the
 * SHELL_CMD / SHELL_CMD_REGISTER macros can take them by reference
 * without a trampoline (which was the previous design — it broke at
 * file scope because GCC braced-group expressions can't appear inside
 * SHELL_STATIC_SUBCMD_SET_CREATE's initializer list).
 *
 * The cost of this approach is that the OSAL handler signature carries
 * `size_t argc` rather than `int argc` — but that's what every modern
 * shell library uses, so it travels well to non-Zephyr backends. */

typedef const struct shell nn_osal_shell_ctx_t;

#  define _NN_OSAL_SHELL_CMD_REGISTER_IMPL(name, handler, help)              \
        SHELL_CMD_REGISTER(name, NULL, help, handler)

#  define _NN_OSAL_SHELL_CMD_IMPL(name, handler, help)                       \
        SHELL_CMD(name, NULL, help, handler)

#  define _NN_OSAL_SHELL_CMD_ARG_IMPL(name, handler, help, mand, opt)        \
        SHELL_CMD_ARG(name, NULL, help, handler, mand, opt)

/* User code must include NN_OSAL_SHELL_SUBCMD_SET_END as the final entry,
 * mirroring Zephyr's SHELL_SUBCMD_SET_END.  We don't auto-append it here
 * because a trailing comma after the last NN_OSAL_SHELL_CMD entry would
 * leave an empty token between the comma and our appended end-marker,
 * which the C preprocessor rejects as "expected expression before ','". */
#  define _NN_OSAL_SHELL_SUBCMD_SET_CREATE_IMPL(name, ...)                   \
        SHELL_STATIC_SUBCMD_SET_CREATE(name, __VA_ARGS__)

#  define _NN_OSAL_SHELL_SUBCMD_SET_END_IMPL  SHELL_SUBCMD_SET_END

#  define _NN_OSAL_SHELL_CMD_REGISTER_SET_IMPL(name, subset, def_h, help)    \
        SHELL_CMD_REGISTER(name, subset, help, def_h)

#elif defined(CONFIG_NN_OSAL_BACKEND_POSIX)
/* Hosted Linux: no interactive shell surface yet — commands come from the
 * process CLI/env.  The ctx aliases stdio so print helpers can compile. */
typedef struct { int unused; } nn_osal_shell_ctx_t;
#elif defined(CONFIG_NN_OSAL_BACKEND_AMEBA)
/* The SDK has a log-service console (component/console).  Until the backend
 * binds to it, the ctx is a placeholder so the print helpers compile -- same
 * position the POSIX backend is in. */
typedef struct { int unused; } nn_osal_shell_ctx_t;
#else
#  error "no nn_osal backend selected (ZEPHYR / ESP_IDF / POSIX / AMEBA)"
#endif
