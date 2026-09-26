/* SPDX-License-Identifier: Apache-2.0 */
/*
 * `net` console command (C6):
 *   net wifi <ssid> <pass>   set + persist Wi-Fi credentials
 *   net host <ip> [port]     set + persist the host endpoint
 *   net start                connect Wi-Fi + start the TCP video sender
 *   net status               show link/stream state
 */
#include "nn_netstream/nn_netstream.h"
#include "esp_console.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmd_net(int argc, char **argv)
{
    if (argc < 2) { printf("usage: net <wifi|host|start|status>\n"); return 1; }
    const char *sub = argv[1];

    if (!strcmp(sub, "wifi") && argc >= 3) {
        nn_netstream_set_wifi(argv[2], argc >= 4 ? argv[3] : "");
        printf("ok (run 'net start')\n");
    } else if (!strcmp(sub, "host") && argc >= 3) {
        nn_netstream_set_host(argv[2], argc >= 4 ? (uint16_t)atoi(argv[3]) : 0);
        printf("ok\n");
    } else if (!strcmp(sub, "start")) {
        printf("%s\n", nn_netstream_start() == ESP_OK ? "starting..." : "failed (set wifi first)");
    } else if (!strcmp(sub, "status")) {
        char s[128]; nn_netstream_status(s, sizeof s); printf("%s\n", s);
    } else {
        printf("usage: net <wifi ssid pass|host ip [port]|start|status>\n");
        return 1;
    }
    return 0;
}

void nn_netstream_cli_register(void)
{
    const esp_console_cmd_t c = {
        .command = "net",
        .help = "Video Wi-Fi/TCP streaming: net <wifi|host|start|status>",
        .func = cmd_net,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
