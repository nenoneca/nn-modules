/* SPDX-License-Identifier: Apache-2.0 */
#include "nn_ctrl/nn_ctrl.h"
#include "nn_sectun/nn_sectun.h"
#include "nn_timesync/nn_timesync.h"
/* nn_prov (hub key/endpoint store) and nn_ota_c6 (OTA orchestration) are the
 * C6 media-network's dependencies.  On the esp-hosted P4 host neither applies:
 * the hub config comes from nn_ctrl_set_hub() and OTA is the P4's own concern.
 * Gate them so nn_ctrl reuses cleanly on both chips (default y on the C6). */
#if CONFIG_NN_CTRL_HAS_PROV
#include "nn_prov/nn_prov.h"
#endif
#if CONFIG_NN_CTRL_HAS_OTA
#include "nn_ota/nn_ota_c6.h"
#endif

#include "esp_console.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_ctrl);
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define OP_PING            0x01
#define OP_STATUS          0x02
#define NN_CTRL_OP_SET_TIME    0x20   /* body: u64 epoch_ms LE  -> set abs clock */
#define NN_CTRL_OP_TIME_STATUS 0x21   /* -> time-sync status string              */
#define OP_UNKNOWN 0xFF

static volatile bool s_up;
static uint32_t s_reqs;

/* Directly-set hub endpoint + pubkey (P4 host / any caller that doesn't use
 * nn_prov).  Takes precedence over the nn_prov-provisioned values. */
static bool     s_hub_set;
static uint8_t  s_hub_pub_cfg[32];
static char     s_hub_host_cfg[40];
static uint16_t s_hub_port_cfg;
/* Optional STATUS-reply producer (the hub reads it to identify/register the
 * device).  On the C6 this defaults to nn_prov_status_str. */
static nn_ctrl_status_fn_t s_status_fn;

void nn_ctrl_set_hub(const uint8_t pub[32], const char *host, uint16_t port)
{
    if (pub)  memcpy(s_hub_pub_cfg, pub, 32);
    if (host) strlcpy(s_hub_host_cfg, host, sizeof s_hub_host_cfg);
    s_hub_port_cfg = port;
    s_hub_set = true;
}

void nn_ctrl_set_status_fn(nn_ctrl_status_fn_t fn) { s_status_fn = fn; }

/* Resolve the hub endpoint + pubkey: prefer the explicitly-set config, else the
 * nn_prov-provisioned values (+ the build-time fallback host). */
static bool resolve_hub(char *host, size_t hcap, uint16_t *port, uint8_t pub[32])
{
    if (s_hub_set) {
        if (s_hub_host_cfg[0] == '\0' || s_hub_port_cfg == 0) return false;
        memcpy(pub, s_hub_pub_cfg, 32);
        strlcpy(host, s_hub_host_cfg, hcap);
        *port = s_hub_port_cfg;
        return true;
    }
#if CONFIG_NN_CTRL_HAS_PROV
    if (!nn_prov_get_hub_pubkey(pub)) return false;
    if (!nn_prov_get_hub_endpoint(host, hcap, port) || host[0] == '\0' || *port == 0) {
        if (CONFIG_NN_CTRL_DEFAULT_HUB_HOST[0] == '\0') return false;
        strlcpy(host, CONFIG_NN_CTRL_DEFAULT_HUB_HOST, hcap);
        *port = CONFIG_NN_CTRL_DEFAULT_HUB_PORT;
        NN_LOG_WRN("control endpoint unprovisioned — fallback %s:%u", host, *port);
    }
    return true;
#else
    return false;
#endif
}

static size_t status_str(char *out, size_t n)
{
    if (s_status_fn) return s_status_fn(out, n);
#if CONFIG_NN_CTRL_HAS_PROV
    nn_prov_status_str(out, n);
    return strlen(out);
#else
    return strlcpy(out, "status=up", n);
#endif
}

#define REQ_CAP  1280   /* fits an OTA DATA record: 6 hdr + 1008 block + margin */
#define REP_CAP  512

#if CONFIG_NN_CTRL_HAS_OTA   /* only the OTA opcodes decode LE u32 offsets/sizes */
static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
#endif

static int tcp_connect(const char *host, uint16_t port)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    /* Same hostname support as nn_netstream's uplink: inet_addr() only parses a
     * dotted-quad, so a provisioned hub endpoint like "opi.local" would never
     * connect.  getaddrinfo() handles both, and lwIP resolves ".local" over
     * mDNS (CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y).  Both endpoints must
     * accept names or the device follows the server on the video uplink but
     * still loses the control channel when the server's address changes. */
    a.sin_addr.s_addr = inet_addr(host);
    if (a.sin_addr.s_addr == INADDR_NONE) {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        char portstr[8];
        snprintf(portstr, sizeof portstr, "%u", (unsigned)port);
        if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
            if (res) freeaddrinfo(res);
            return -1;
        }
        a.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    struct timeval to = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

/* Build a reply for a request.  Returns reply length. */
static size_t dispatch(const uint8_t *req, size_t rn, uint8_t *rep, size_t cap)
{
    uint8_t op = (rn >= 1) ? req[0] : OP_UNKNOWN;
    switch (op) {
    case OP_PING:
        rep[0] = OP_PING;
        memcpy(rep + 1, "pong", 4);
        return 5;
    case OP_STATUS: {
        rep[0] = OP_STATUS;
        char line[200];
        status_str(line, sizeof line);
        size_t n = strlcpy((char *)rep + 1, line, cap - 1);
        if (n > cap - 1) n = cap - 1;
        return 1 + n;
    }

#if CONFIG_NN_CTRL_HAS_OTA
    /* ── OTA control (hub-driven; see nn_ota_c6) ─────────────────────────── */
    case NN_OTA_OP_OFFER:
        rep[0] = op; rep[1] = (uint8_t)nn_ota_c6_offer(req + 1, rn - 1);
        return 2;
    case NN_OTA_OP_BEGIN:
        if (rn < 6) { rep[0] = op; rep[1] = 0xFE; return 2; }
        rep[0] = op; rep[1] = (uint8_t)nn_ota_c6_begin(req[1], rd_u32le(req + 2));
        return 2;
    case NN_OTA_OP_DATA:
        if (rn < 6) { rep[0] = op; rep[1] = 0xFE; return 2; }
        rep[0] = op;
        rep[1] = (uint8_t)nn_ota_c6_data(req[1], rd_u32le(req + 2), req + 6, rn - 6);
        return 2;
    case NN_OTA_OP_VERIFY:
        if (rn < 6 + 32) { rep[0] = op; rep[1] = 0xFE; return 2; }
        rep[0] = op;
        rep[1] = (uint8_t)nn_ota_c6_verify(req[1], rd_u32le(req + 2), req + 6);
        return 2;
    case NN_OTA_OP_ARM:
        rep[0] = op; rep[1] = (uint8_t)nn_ota_c6_arm();
        return 2;
    case NN_OTA_OP_APPLY:
        rep[0] = op; rep[1] = (uint8_t)nn_ota_c6_apply();
        return 2;
    case NN_OTA_OP_STATUS: {
        rep[0] = op;
        size_t n = nn_ota_c6_status((char *)rep + 1, cap - 1);
        return 1 + n;
    }
#endif /* CONFIG_NN_CTRL_HAS_OTA */

    /* ── time sync (hub -> device, every H hours) ─────────────────────────── */
    case NN_CTRL_OP_SET_TIME: {
        rep[0] = op;
        if (rn < 1 + 8) { rep[1] = 0xFE; return 2; }
        uint64_t epoch = 0;
        for (int i = 0; i < 8; i++) epoch |= (uint64_t)req[1 + i] << (8 * i);
        nn_timesync_set_epoch(epoch);
        rep[1] = 0;
        return 2;
    }
    case NN_CTRL_OP_TIME_STATUS: {
        rep[0] = op;
        char line[120]; nn_timesync_status(line, sizeof line);
        size_t k = strlcpy((char *)rep + 1, line, cap - 1);
        return 1 + (k > cap - 1 ? cap - 1 : k);
    }

    default:
        rep[0] = OP_UNKNOWN;
        return 1;
    }
}

static void ctrl_task(void *arg)
{
    (void)arg;
    char host[40];
    uint16_t port;
    uint8_t hub_pub[32];

    uint8_t *req = malloc(REQ_CAP), *rep = malloc(REP_CAP);
    if (!req || !rep) { NN_LOG_ERR("ctrl buffers OOM"); free(req); free(rep); vTaskDelete(NULL); return; }

    for (;;) {
        /* Resolve hub endpoint + pubkey (explicit set_hub, else nn_prov). */
        if (!resolve_hub(host, sizeof host, &port, hub_pub)) {
            vTaskDelay(pdMS_TO_TICKS(3000));     /* not configured yet */
            continue;
        }

        int fd = tcp_connect(host, port);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(3000)); continue; }

        nn_sectun_t s;
        if (nn_sectun_client_handshake(&s, fd, hub_pub) != ESP_OK) {
            close(fd); vTaskDelay(pdMS_TO_TICKS(3000)); continue;
        }
        NN_LOG_INF("control channel up to hub %s:%u (encrypted)", host, port);
        s_up = true;

        for (;;) {
            size_t rn = 0, rl;
            if (nn_sectun_recv(&s, req, REQ_CAP, &rn) != ESP_OK) break;
            s_reqs++;
            rl = dispatch(req, rn, rep, REP_CAP);
            if (nn_sectun_send(&s, rep, rl) != ESP_OK) break;
        }

        NN_LOG_WRN("control channel dropped; reconnecting");
        s_up = false;
        close(fd);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static int cmd_ctrl(int argc, char **argv)
{
    (void)argc; (void)argv;
    char host[40] = "(unset)"; uint16_t port = 0; uint8_t pub[32];
    resolve_hub(host, sizeof host, &port, pub);
    printf("control: link=%s hub=%s:%u reqs_served=%lu\n",
           s_up ? "up" : "down", host, port, (unsigned long)s_reqs);
    return 0;
}

esp_err_t nn_ctrl_init(void)
{
    const esp_console_cmd_t c = { .command = "ctrl", .help = "Hub control channel status", .func = cmd_ctrl };
    esp_console_cmd_register(&c);
    return ESP_OK;
}

esp_err_t nn_ctrl_start(void)
{
    /* 8 KB: ctrl_task holds nn_sectun's ~4 KB record buffer + req/rep on stack. */
    return xTaskCreate(ctrl_task, "nn_ctrl", 8192, NULL, 7, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}
