/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_camera_diag — WebSocket transport for the ISP diagnostic mode.
 *
 * Kept in its OWN translation unit because esp_websocket_client.h drags in
 * lwip/sockets.h, whose _IO/_IOR/_IOW macros clash (redefined, -Werror) with
 * linux/videodev2.h's in nn_camera.c.  nn_camera.c owns the V4L2 capture and
 * calls these thin wrappers; the two header worlds never meet.
 */
#include "sdkconfig.h"
#if CONFIG_NN_CAMERA_DIAG_ISP
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>

void *nn_cam_diag_open(const char *uri)
{
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .buffer_size = 17 * 1024,   /* >= the app's send chunk */
        .task_stack = 6144,
    };
    esp_websocket_client_handle_t c = esp_websocket_client_init(&cfg);
    if (c && esp_websocket_client_start(c) != ESP_OK) {
        esp_websocket_client_destroy(c);
        return NULL;
    }
    return c;
}

bool nn_cam_diag_connected(void *h)
{
    return h && esp_websocket_client_is_connected((esp_websocket_client_handle_t)h);
}

int nn_cam_diag_text(void *h, const char *s, int len, int to_ms)
{
    return esp_websocket_client_send_text((esp_websocket_client_handle_t)h, s, len,
                                          pdMS_TO_TICKS(to_ms));
}

int nn_cam_diag_bin(void *h, const void *d, int len, int to_ms)
{
    return esp_websocket_client_send_bin((esp_websocket_client_handle_t)h, (const char *)d, len,
                                         pdMS_TO_TICKS(to_ms));
}

void nn_cam_diag_close(void *h)
{
    if (h) {
        esp_websocket_client_stop((esp_websocket_client_handle_t)h);
        esp_websocket_client_destroy((esp_websocket_client_handle_t)h);
    }
}
#endif /* CONFIG_NN_CAMERA_DIAG_ISP */
