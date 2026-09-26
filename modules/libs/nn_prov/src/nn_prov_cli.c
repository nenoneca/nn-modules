/* SPDX-License-Identifier: Apache-2.0 */
#include "nn_prov/nn_prov.h"
#include "esp_console.h"
#include <nn_osal/storage.h>
#include <stdio.h>
#include <string.h>

static int cmd_prov(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "pubkey") == 0) {
        uint8_t pub[32];
        nn_prov_get_device_pubkey(pub);
        printf("device X25519 pub: ");
        for (int i = 0; i < 32; i++) printf("%02x", pub[i]);
        printf("\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        /* Keep the device keypair; clear only the provisioned config.
         * gw_blob is deliberately kept too — it is delivered once and is
         * not part of "this camera's provisioning". */
        static const char *const keys[] = {
            "nnprov/name", "nnprov/hub_pub", "nnprov/stream_pub",
            "nnprov/hub_host", "nnprov/hub_port", "nnprov/done",
        };
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
            (void)nn_osal_kv_delete(keys[i]);
        printf("provisioning config cleared (device key kept) — reboot to take effect\n");
        return 0;
    }
    char line[200];
    nn_prov_status_str(line, sizeof line);
    printf("%s\n", line);
    printf("usage: prov [pubkey|reset]\n");
    return 0;
}

void nn_prov_cli_register(void)
{
    const esp_console_cmd_t c = {
        .command = "prov",
        .help = "BLE provisioning: 'prov' status, 'prov pubkey', 'prov reset'",
        .func = cmd_prov,
    };
    esp_console_cmd_register(&c);
}
