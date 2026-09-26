/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>

/*
 * nn_osal/util.h — tiny portable helper macros.
 *
 * Replaces the handful of <zephyr/sys/util.h> macros nn libs reach for:
 *   ARRAY_SIZE, MIN, MAX, CLAMP, ARG_UNUSED.
 *
 * Kept as macros (not static inlines) so they work on array-typed
 * arguments and at file scope.
 */

#ifndef NN_OSAL_ARRAY_SIZE
#define NN_OSAL_ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef NN_OSAL_MIN
#define NN_OSAL_MIN(a, b)       ((a) < (b) ? (a) : (b))
#endif

#ifndef NN_OSAL_MAX
#define NN_OSAL_MAX(a, b)       ((a) > (b) ? (a) : (b))
#endif

#ifndef NN_OSAL_CLAMP
#define NN_OSAL_CLAMP(x, lo, hi) \
    ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#endif

#ifndef NN_OSAL_UNUSED
#define NN_OSAL_UNUSED(x)       ((void)(x))
#endif
