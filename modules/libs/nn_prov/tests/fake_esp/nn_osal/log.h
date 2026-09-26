/* SPDX-License-Identifier: Apache-2.0 */
/* Host-test stand-in for nn_osal/log.h: printf-backed. */
#pragma once
#include <stdio.h>
#define NN_OSAL_LOG_MODULE(name)
#define NN_LOG_ERR(...) do { printf("E: " __VA_ARGS__); printf("\n"); } while (0)
#define NN_LOG_WRN(...) do { printf("W: " __VA_ARGS__); printf("\n"); } while (0)
#define NN_LOG_INF(...) do { printf("I: " __VA_ARGS__); printf("\n"); } while (0)
#define NN_LOG_DBG(...) do { } while (0)
