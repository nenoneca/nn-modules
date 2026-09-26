/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/*
 * Logging is the one OSAL surface where macros need to expand into
 * backend-native LOG_INF/LOG_WRN/etc. so the backend's compile-time
 * level filtering, per-module overrides, and immediate-vs-deferred
 * settings all keep working.
 *
 * Each source file declares its tag via:
 *   NN_OSAL_LOG_MODULE("my_mod");
 * which on Zephyr expands to LOG_MODULE_REGISTER + a #define for the
 * tag string that the NN_LOG_* macros pick up.
 */

#ifdef CONFIG_NN_OSAL_BACKEND_ZEPHYR
#  include <zephyr/logging/log.h>

#  define _NN_OSAL_LOG_MODULE_IMPL(name)                                  \
        LOG_MODULE_REGISTER(name, CONFIG_NN_OSAL_LOG_LEVEL);              \
        __attribute__((unused))                                           \
        static const char _NN_OSAL_LOG_TAG[] = #name

   /* Override the generic emit-based macros from log.h with Zephyr-
    * native ones so call-site level filtering still happens at compile
    * time, not inside nn_osal_log_emit. */
#  undef  NN_LOG_INF
#  undef  NN_LOG_WRN
#  undef  NN_LOG_ERR
#  undef  NN_LOG_DBG
#  define NN_LOG_INF(...) LOG_INF(__VA_ARGS__)
#  define NN_LOG_WRN(...) LOG_WRN(__VA_ARGS__)
#  define NN_LOG_ERR(...) LOG_ERR(__VA_ARGS__)
#  define NN_LOG_DBG(...) LOG_DBG(__VA_ARGS__)
#elif defined(CONFIG_NN_OSAL_BACKEND_AMEBA)
/* No native level-filtering macros to map onto, so the generic emit-based
 * NN_LOG_* macros in log.h are used as-is and the module name is just a tag
 * string.  Filtering therefore happens inside nn_osal_log_emit rather than at
 * the call site -- a real cost on an MCU, and worth revisiting if log volume
 * shows up in profiling. */
#  define _NN_OSAL_LOG_MODULE_IMPL(name)                                  \
        __attribute__((unused))                                           \
        static const char _NN_OSAL_LOG_TAG[] = #name
#else
   /* Fallback: tag is a plain string, macros stay generic. */
#  define _NN_OSAL_LOG_MODULE_IMPL(name)                                  \
        __attribute__((unused))                                           \
        static const char _NN_OSAL_LOG_TAG[] = #name
#endif
