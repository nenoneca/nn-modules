/* SPDX-License-Identifier: Apache-2.0 */
#include "nn_registry/nn_features.h"
#include <stdio.h>

const char *nn_registry_summary(void)
{
    static char buf[80];
    snprintf(buf, sizeof buf, "%s [%s%s%s%s]",
             NN_APP_NAME,
             NN_LINK_IS_MASTER ? "master " : (NN_LINK_IS_SLAVE ? "slave " : ""),
             NN_HAS_WIFI       ? "wifi "   : "",
             NN_HAS_BLE        ? "ble "    : "",
             NN_HAS_P4CTL      ? "p4ctl"   : "");
    /* trim a trailing space if no p4ctl */
    size_t n = 0;
    while (buf[n]) n++;
    if (n >= 2 && buf[n - 2] == ' ') { buf[n - 2] = ']'; buf[n - 1] = '\0'; }
    return buf;
}
