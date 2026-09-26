/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/shell.h>
#include <zephyr/shell/shell.h>
#include <stdarg.h>

/* The "opaque" shell context is literally Zephyr's `const struct shell *`.
 * The header's nn_osal_shell_ctx_t* is just that pointer cast — no
 * conversion needed at the boundary. */

static const struct shell *to_zsh(nn_osal_shell_ctx_t *sh)
{
    return (const struct shell *)sh;
}

void nn_osal_shell_print(nn_osal_shell_ctx_t *sh, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    shell_vfprintf(to_zsh(sh), SHELL_NORMAL, fmt, ap);
    shell_fprintf(to_zsh(sh), SHELL_NORMAL, "\n");
    va_end(ap);
}

void nn_osal_shell_error(nn_osal_shell_ctx_t *sh, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    shell_vfprintf(to_zsh(sh), SHELL_ERROR, fmt, ap);
    shell_fprintf(to_zsh(sh), SHELL_ERROR, "\n");
    va_end(ap);
}

void nn_osal_shell_warn(nn_osal_shell_ctx_t *sh, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    shell_vfprintf(to_zsh(sh), SHELL_WARNING, fmt, ap);
    shell_fprintf(to_zsh(sh), SHELL_WARNING, "\n");
    va_end(ap);
}

void nn_osal_shell_print_v(nn_osal_shell_ctx_t *sh, const char *fmt, va_list ap)
{
    shell_vfprintf(to_zsh(sh), SHELL_NORMAL, fmt, ap);
}
