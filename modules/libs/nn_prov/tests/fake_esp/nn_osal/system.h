/* SPDX-License-Identifier: Apache-2.0 */
/* Fake nn_osal/system.h — counts reboots instead of performing one.
 * NOT marked noreturn (unlike the real header) precisely so it can return
 * into the test. */
#pragma once

typedef enum {
    NN_OSAL_REBOOT_COLD = 0,
    NN_OSAL_REBOOT_WARM = 1,
} nn_osal_reboot_type_t;

void nn_osal_sys_reboot(nn_osal_reboot_type_t type);
