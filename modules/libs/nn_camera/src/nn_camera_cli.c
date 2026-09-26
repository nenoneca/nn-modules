/* SPDX-License-Identifier: Apache-2.0 */
#include "nn_camera/nn_camera.h"
#include "esp_console.h"
#include <stdio.h>
#include <string.h>

static int cmd_cam(int argc, char **argv)
{
    if (argc < 2) { printf("usage: cam <info|scan|start|stop|stats>\n"); return 1; }
    const char *sub = argv[1];
    if (!strcmp(sub, "info")) {
        printf("sensor: %s\n", nn_camera_sensor_name());
    } else if (!strcmp(sub, "scan")) {
        printf("%s\n", nn_camera_i2c_scan() == ESP_OK ? "found device(s)" : "no I2C devices");
    } else if (!strcmp(sub, "start")) {
        printf("%s\n", nn_camera_start() == ESP_OK ? "started" : "start failed");
    } else if (!strcmp(sub, "stop")) {
        nn_camera_stop(); printf("stopped\n");
    } else if (!strcmp(sub, "stats")) {
        nn_camera_stats_t s; nn_camera_get_stats(&s);
        printf("res=%dx%d frames=%lu key=%lu last=%lu bytes=%llu drops=%lu csi_get=%lu csi_done=%lu\n",
               s.width, s.height, (unsigned long)s.frames, (unsigned long)s.keyframes,
               (unsigned long)s.last_size, (unsigned long long)s.total_bytes,
               (unsigned long)s.drops, (unsigned long)s.csi_get, (unsigned long)s.csi_done);
    } else { printf("unknown: %s\n", sub); return 1; }
    return 0;
}

void nn_camera_cli_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "cam",
        .help = "Camera: cam <info|start|stop|stats>",
        .func = cmd_cam,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
