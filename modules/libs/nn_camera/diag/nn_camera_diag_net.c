/* TCP sender for the ISP pipeline tap, isolated in its own translation unit.
 *
 * WHY SEPARATE: lwip/sockets.h and esp_video's linux/ioctl.h both define _IO/
 * _IOR/_IOW, so including both in nn_camera.c is a -Werror redefinition fight.
 * Keeping the socket code here means nn_camera.c needs no lwIP headers at all.
 */
#include <string.h>
#include <stdio.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_err.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_cam_diagnet);

esp_err_t nn_camera_diag_net_send(const char *host, uint16_t port,
                                  const void *hdr, size_t hdr_len,
                                  const void *data, size_t data_len)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return ESP_FAIL;

    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = inet_addr(host);
    if (a.sin_addr.s_addr == INADDR_NONE) {          /* hostname, incl. mDNS */
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        char ps[8];
        snprintf(ps, sizeof ps, "%u", (unsigned)port);
        if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) {
            if (res) freeaddrinfo(res);
            close(fd);
            NN_LOG_ERR("resolve %s failed", host);
            return ESP_FAIL;
        }
        a.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    struct timeval to = { .tv_sec = 20 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        NN_LOG_ERR("connect %s:%u failed", host, (unsigned)port);
        close(fd);
        return ESP_FAIL;
    }

    if (send(fd, hdr, hdr_len, 0) != (int)hdr_len) {
        close(fd);
        return ESP_FAIL;
    }
    size_t off = 0;
    while (off < data_len) {
        int wr = send(fd, (const uint8_t *)data + off, data_len - off, 0);
        if (wr <= 0) break;
        off += (size_t)wr;
    }
    close(fd);
    if (off != data_len) {
        NN_LOG_WRN("short send: %u/%u B", (unsigned)off, (unsigned)data_len);
        return ESP_FAIL;
    }
    return ESP_OK;
}
