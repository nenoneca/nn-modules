/* SPDX-License-Identifier: Apache-2.0 */
/*
 * `p4` console command (C6 side):
 *   p4 power-cycle   — reboot P4 into normal mode
 *   p4 download      — reboot P4 into ROM download mode
 *   p4 reset         — hold-then-release reset (alias of power-cycle)
 *   p4 hold          — assert reset (keep P4 held)
 *   p4 release       — release reset
 */
#include "nn_p4ctl/nn_p4ctl.h"
#include "esp_console.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static int cmd_p4(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: p4 <power-cycle|download|reset|hold|release>\n");
        return 1;
    }
    const char *sub = argv[1];
    esp_err_t ret = ESP_OK;

    if (!strcmp(sub, "power-cycle") || !strcmp(sub, "reset")) {
        ret = nn_p4ctl_power_cycle();
    } else if (!strcmp(sub, "download")) {
        ret = nn_p4ctl_enter_download();
    } else if (!strcmp(sub, "hold")) {
        ret = nn_p4ctl_assert_reset();
    } else if (!strcmp(sub, "release")) {
        ret = nn_p4ctl_release_reset();
    } else {
        printf("unknown subcommand '%s'\n", sub);
        return 1;
    }
    printf("%s: %s\n", sub, esp_err_to_name(ret));
    return ret == ESP_OK ? 0 : 1;
}

void nn_p4ctl_cli_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "p4",
        .help    = "Control the P4 boot/reset: p4 <power-cycle|download|reset|hold|release>",
        .func    = cmd_p4,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
