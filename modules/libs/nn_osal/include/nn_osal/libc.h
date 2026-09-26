/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdarg.h>

/*
 * nn_osal/libc.h — wrappers for the libc functions the nn libs use.
 *
 * Rationale: most targets ship a libc, but a few (deeply constrained
 * baremetal builds, formal verification tools, custom secure runtimes)
 * don't.  Wrapping forces every call site to go through this header so
 * a port to such an environment is a single-file change.
 *
 * Wrappers are static-inline pass-throughs by default; the backend can
 * override any of them by defining NN_OSAL_LIBC_OVERRIDE_<name> before
 * this header is included.  Backends that want to keep libc untouched
 * need do nothing.
 */

#include "internal/backend_libc.h"   /* may set NN_OSAL_LIBC_OVERRIDE_* */

/* If a backend hasn't overridden a wrapper, fall through to the
 * standard libc one. */

#ifndef NN_OSAL_LIBC_OVERRIDE_memcpy
#include <string.h>
static inline void *nn_osal_memcpy(void *d, const void *s, size_t n)
    { return memcpy(d, s, n); }
#endif

#ifndef NN_OSAL_LIBC_OVERRIDE_memset
#include <string.h>
static inline void *nn_osal_memset(void *d, int c, size_t n)
    { return memset(d, c, n); }
#endif

#ifndef NN_OSAL_LIBC_OVERRIDE_memcmp
#include <string.h>
static inline int nn_osal_memcmp(const void *a, const void *b, size_t n)
    { return memcmp(a, b, n); }
#endif

#ifndef NN_OSAL_LIBC_OVERRIDE_strlen
#include <string.h>
static inline size_t nn_osal_strlen(const char *s)
    { return strlen(s); }
#endif

#ifndef NN_OSAL_LIBC_OVERRIDE_strcmp
#include <string.h>
static inline int nn_osal_strcmp(const char *a, const char *b)
    { return strcmp(a, b); }
#endif

#ifndef NN_OSAL_LIBC_OVERRIDE_strncmp
#include <string.h>
static inline int nn_osal_strncmp(const char *a, const char *b, size_t n)
    { return strncmp(a, b, n); }
#endif

#ifndef NN_OSAL_LIBC_OVERRIDE_strncpy
#include <string.h>
static inline char *nn_osal_strncpy(char *d, const char *s, size_t n)
    { return strncpy(d, s, n); }
#endif

#ifndef NN_OSAL_LIBC_OVERRIDE_strchr
#include <string.h>
static inline char *nn_osal_strchr(const char *s, int c)
    { return strchr(s, c); }
#endif

#ifndef NN_OSAL_LIBC_OVERRIDE_strstr
#include <string.h>
static inline char *nn_osal_strstr(const char *h, const char *n)
    { return strstr(h, n); }
#endif

#ifndef NN_OSAL_LIBC_OVERRIDE_atoi
#include <stdlib.h>
static inline int nn_osal_atoi(const char *s)
    { return atoi(s); }
#endif

#ifndef NN_OSAL_LIBC_OVERRIDE_snprintf
#include <stdio.h>
__attribute__((format(printf, 3, 4)))
static inline int nn_osal_snprintf(char *b, size_t n, const char *f, ...) {
    va_list ap; va_start(ap, f);
    int r = vsnprintf(b, n, f, ap);
    va_end(ap);
    return r;
}
#endif

#ifndef NN_OSAL_LIBC_OVERRIDE_sscanf
#include <stdio.h>
/* sscanf's variadic-with-format is messy through a static inline
 * because the variadic forwarder vsscanf-equivalent doesn't exist in
 * portable C.  Use a macro that drops to the real sscanf, keeping
 * call-sites still going through the nn_osal_* spelling. */
#define nn_osal_sscanf(buf, fmt, ...)  sscanf((buf), (fmt), __VA_ARGS__)
#endif
