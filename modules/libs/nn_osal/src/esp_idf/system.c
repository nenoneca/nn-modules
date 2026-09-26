/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal system backend — ESP-IDF. */
#include "nn_osal/system.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include <stdlib.h>

void nn_osal_sys_reboot(nn_osal_reboot_type_t type)
{
    (void)type;          /* esp_restart() is a full digital-core reset (COLD) */
    esp_restart();
    for (;;) { }         /* unreachable — keeps the compiler happy */
}

void nn_osal_sys_panic(const char *reason)
{
    (void)reason;
    abort();
}

const char *nn_osal_app_version(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    return d ? d->version : NULL;
}
