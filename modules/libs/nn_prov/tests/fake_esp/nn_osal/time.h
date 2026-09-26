/* SPDX-License-Identifier: Apache-2.0 */
/* Fake nn_osal/time.h — returns immediately so the suite stays fast. */
#pragma once
#include <stdint.h>
uint32_t nn_osal_sleep_ms(uint32_t ms);
