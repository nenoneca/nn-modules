/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/*
 * Zephyr ships with picolibc (or newlib).  No overrides needed — the
 * static-inline wrappers in nn_osal/libc.h pass straight through to
 * libc, which is exactly what we want on a hosted target.
 *
 * Define NN_OSAL_LIBC_OVERRIDE_<name> here (and provide the impl in
 * the matching .c file under src/<backend>/) to redirect any wrapper
 * to a backend-specific implementation.
 */
