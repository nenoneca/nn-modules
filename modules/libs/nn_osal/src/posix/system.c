/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal system backend — POSIX.
 * "Reboot" on a hosted service means exit and let the supervisor (systemd
 * Restart=) bring us back — same recovery contract as a device reset. */
#include "nn_osal/system.h"
#include <stdio.h>
#include <stdlib.h>

void nn_osal_sys_reboot(nn_osal_reboot_type_t type)
{
    (void)type;
    fprintf(stderr, "nn_osal(posix): reboot requested — exiting for supervisor restart\n");
    exit(75);          /* EX_TEMPFAIL: systemd restarts on-failure units */
}

void nn_osal_sys_panic(const char *reason)
{
    fprintf(stderr, "nn_osal(posix): PANIC: %s\n", reason ? reason : "?");
    abort();
}

const char *nn_osal_app_version(void)
{
    const char *v = getenv("NN_APP_VERSION");
    return v ? v : "dev";
}
