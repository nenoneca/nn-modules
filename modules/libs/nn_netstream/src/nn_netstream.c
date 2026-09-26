/* SPDX-License-Identifier: Apache-2.0 */
#include "nn_netstream/nn_netstream.h"
#include "nn_sectun/nn_sectun.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_netstream);
#include <nn_pal/wifi.h>           /* STA bring-up behind the PAL */
#include <fcntl.h>
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"           /* EXT_RAM_BSS_ATTR for PSRAM static placement */
#include "esp_memory_utils.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>


#define NVS_NS        "nnstream"
/* H.264 byte-stream FIFO between the encoder (producer) and the TCP/sectun drain
 * task (consumer).  Backed by PSRAM (see nn_netstream_init) so it can be large
 * without competing with esp-hosted's internal-only DMA mempool.
 *
 * Sweep (2026-07-12, 3 Mbps encoder, SDIO 4-bit 8MHz): 48K→512K→2M cut drops ~4x
 * and lifted sustained throughput 856→~2200 kbit/s (mostly by ending drain-task
 * starvation).  512 KB captures the bulk of that; beyond it the gains are within
 * run-to-run Wi-Fi noise.  BUT the encrypted drain ceiling itself swings
 * ~1.4–2.2 Mbps (Wi-Fi air + esp-hosted RTT), so buffer alone NEVER reaches zero
 * drops — the source bitrate must be adapted below the live drain rate.  That's
 * the job of the server->device control channel (adaptive bitrate + force-IDR). */
#ifndef NNS_SBUF_KB
/* Static internal-BSS buffer sized to fit the smallest target (ESP32-C6,
 * ~340 KB heap, no PSRAM).  64 KB is ample smoothing (~256 ms at 2 Mbps) and
 * keeps the buffer in fast internal RAM for both the C6 uplink and any
 * PSRAM-having target (which links nn_netstream but doesn't run the uplink). */
#define NNS_SBUF_KB   64
#endif
#define SBUF_SIZE     (NNS_SBUF_KB * 1024)
/* Drain in big chunks: one chunk == one max nn_sectun record (4 KB plaintext),
 * so each TCP send is one AES-GCM op instead of ~3 — ~2.8x fewer crypto+framing
 * ops per byte, which is the C6->host uplink bottleneck.  (Must be <= the sectun
 * record max so a chunk never splits into two sectun records.) */
#define TCP_CHUNK     4096

static char     s_ssid[33];
static char     s_pass[65];
/* The uplink host comes from provisioning (NVS "host", set over BLE by the
 * hub).  No address is baked in; a build may still pin a bench default with
 * -DNN_NETSTREAM_DEFAULT_HOST="\"host\"" (never commit a real one). */
#ifndef NN_NETSTREAM_DEFAULT_HOST
#define NN_NETSTREAM_DEFAULT_HOST ""
#endif
static char     s_host[40] = NN_NETSTREAM_DEFAULT_HOST;
static uint16_t s_port = 8888;
static int s_conn_fails, s_reassoc_rounds;   /* uplink reconnect escalation */

static StreamBufferHandle_t s_sbuf;
static volatile bool s_have_ip;
static volatile bool s_tcp_up;
static int  s_sock = -1;
static uint32_t s_tx_bytes, s_tx_drops;
static uint32_t s_drop_notcp, s_drop_nospace, s_rec_writes;

/* Encrypted uplink: when the stream service pubkey is provisioned, every byte
 * goes through an nn_sectun session instead of plaintext TCP. */
static uint8_t     s_stream_pub[32];
static bool        s_have_stream_key;
static nn_sectun_t s_tun;
static bool        s_secure;

void nn_netstream_set_stream_key(const uint8_t pub[32])
{
    memcpy(s_stream_pub, pub, 32);
    s_have_stream_key = true;
    NN_LOG_INF("stream key set (%02x%02x%02x%02x..) — uplink will be encrypted",
             pub[0], pub[1], pub[2], pub[3]);
}

bool nn_netstream_is_streaming(void) { return s_have_ip && s_tcp_up; }

uint32_t nn_netstream_get_nospace_drops(void) { return s_drop_nospace; }

/* ── NVS config ──────────────────────────────────────────────────────── */
static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof s_ssid; nvs_get_str(h, "ssid", s_ssid, &n);
    n = sizeof s_pass;        nvs_get_str(h, "pass", s_pass, &n);
    n = sizeof s_host;        nvs_get_str(h, "host", s_host, &n);
    uint16_t p; if (nvs_get_u16(h, "port", &p) == ESP_OK) s_port = p;
    nvs_close(h);
}

esp_err_t nn_netstream_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    strlcpy(s_ssid, ssid, sizeof s_ssid);
    strlcpy(s_pass, pass ? pass : "", sizeof s_pass);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    nvs_set_str(h, "ssid", s_ssid); nvs_set_str(h, "pass", s_pass);
    nvs_commit(h); nvs_close(h);
    NN_LOG_INF("wifi creds set: ssid='%s'", s_ssid);
    return ESP_OK;
}

esp_err_t nn_netstream_set_host(const char *ip, uint16_t port)
{
    if (ip) strlcpy(s_host, ip, sizeof s_host);
    if (port) s_port = port;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    nvs_set_str(h, "host", s_host); nvs_set_u16(h, "port", s_port);
    nvs_commit(h); nvs_close(h);
    NN_LOG_INF("host set: %s:%u", s_host, s_port);
    return ESP_OK;
}

/* ── Wi-Fi STA (via nn_pal/wifi) ──────────────────────────────────────── */
static void wifi_cb(nn_pal_wifi_event_t ev, const nn_pal_wifi_state_t *st, void *user)
{
    (void)user;
    if (ev == NN_PAL_WIFI_EV_IP_ASSIGNED) {
        NN_LOG_INF("wifi got IP %s", st->ipv4);
        s_have_ip = true;
    } else if (ev == NN_PAL_WIFI_EV_DISCONNECTED) {
        s_have_ip = false;
        NN_LOG_WRN("wifi disconnected, retrying");
    }
    /* nn_pal auto-(re)connects internally; WIFI_PS_NONE is set in connect. */
}

static esp_err_t wifi_start(void)
{
    if (s_ssid[0] == '\0') { NN_LOG_ERR("no SSID — use 'net wifi <ssid> <pass>'"); return ESP_ERR_INVALID_STATE; }
    nn_pal_wifi_init(wifi_cb, NULL);
    if (nn_pal_wifi_connect(s_ssid, s_pass) != 0) return ESP_FAIL;
    NN_LOG_INF("wifi STA connecting to '%s' (ps=none)...", s_ssid);
    return ESP_OK;
}

/* ── TCP sender ──────────────────────────────────────────────────────── */
static int tcp_connect(void)
{
    if (!s_host[0]) {                 /* not provisioned: nothing to dial */
        static bool warned;
        if (!warned) { NN_LOG_WRN("no stream host provisioned: uplink idle"); warned = true; }
        return -1;
    }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(s_port) };
    /* Accept a HOSTNAME as well as a dotted-quad.  inet_addr() parses only the
     * latter and returns INADDR_NONE for anything else, so a provisioned host
     * like "opi.local" could never connect.  With
     * CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y (already set in the camera builds)
     * lwIP resolves ".local" over mDNS, so the camera FOLLOWS the server's
     * address instead of being pinned to whatever IP it was provisioned with —
     * a DHCP change on the server silently took the whole fleet offline before.
     * Resolution is per connect attempt, which is what we want: the reconnect
     * loop then re-resolves after the server moves. */
    a.sin_addr.s_addr = inet_addr(s_host);
    if (a.sin_addr.s_addr == INADDR_NONE) {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        char portstr[8];
        snprintf(portstr, sizeof portstr, "%u", (unsigned)s_port);
        int rc = getaddrinfo(s_host, portstr, &hints, &res);
        if (rc != 0 || res == NULL) {
            NN_LOG_WRN("resolve '%s' failed (rc=%d)", s_host, rc);
            if (res) freeaddrinfo(res);
            return -1;
        }
        a.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    /* A transient Wi-Fi/coex stall must NOT tear down the whole encrypted
     * session: a teardown loses keyframe sync and triggers a multi-second
     * reconnect + rehandshake, which is the real fps killer.  Ride through brief
     * congestion (up to 20 s) and let TCP keepalive prove the peer is truly dead
     * rather than a 5 s send-timeout nuking a momentarily-congested link. */
    struct timeval to = { .tv_sec = 20 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
    int ka = 1, kidle = 5, kintvl = 3, kcnt = 4;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof ka);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE,  &kidle,  sizeof kidle);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &kintvl, sizeof kintvl);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT,   &kcnt,   sizeof kcnt);
    /* Non-blocking connect with a 10 s cap: when the AP association is stale
     * (Wi-Fi thinks it's up but the path is dead), a blocking connect burns
     * ~60 s per attempt in SYN retries and the reconnect loop crawls. */
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(s, (struct sockaddr *)&a, sizeof a);
    if (rc != 0 && errno == EINPROGRESS) {
        fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
        struct timeval cto = { .tv_sec = 10 };
        rc = (select(s + 1, NULL, &wf, NULL, &cto) == 1) ? 0 : -1;
        if (rc == 0) {
            int soerr = 0; socklen_t sl = sizeof soerr;
            getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            if (soerr != 0) rc = -1;
        }
    }
    if (rc != 0) { close(s); return -1; }
    fcntl(s, F_SETFL, fl);              /* back to blocking for send/recv */
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;
}

static void net_task(void *arg)
{
    (void)arg;
    static uint8_t chunk[TCP_CHUNK];   /* off-stack: 4 KB + sectun's 4 KB > stack */
    for (;;) {
        while (!s_have_ip) vTaskDelay(pdMS_TO_TICKS(200));

        s_sock = tcp_connect();
        if (s_sock < 0) {
            NN_LOG_WRN("tcp connect %s:%u failed", s_host, s_port);
            /* A stale association reports ip=yes while every packet dies.  After
             * ~1 min of failures, force a Wi-Fi re-associate to recover. */
            /* counters at file scope so success resets them */
            if (++s_conn_fails >= 5) {
                s_conn_fails = 0;
                /* Escalation: if re-association hasn't restored the path after
                 * ~3 rounds (~3 min), the C6 radio co-processor itself is
                 * usually wedged (rpc_core "Timeout waiting for Resp") — only a
                 * hard slave reset recovers it, and the host pulses the C6
                 * reset GPIO during boot, so reboot ourselves. */
                if (++s_reassoc_rounds > 3) {
                    NN_LOG_ERR("radio unrecoverable after %d re-assoc rounds — rebooting to reset the C6", s_reassoc_rounds - 1);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                }
                NN_LOG_WRN("repeated connect failures — re-associating Wi-Fi (round %d)", s_reassoc_rounds);
                nn_pal_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(500));
                nn_pal_wifi_connect(s_ssid, s_pass);
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        /* If provisioned with the stream service's pubkey, establish an
         * encrypted nn_sectun session before sending any video. */
        s_secure = false;
        if (s_have_stream_key) {
            if (nn_sectun_client_handshake(&s_tun, s_sock, s_stream_pub) != ESP_OK) {
                NN_LOG_WRN("secure handshake failed — reconnecting");
                close(s_sock); s_sock = -1; vTaskDelay(pdMS_TO_TICKS(2000)); continue;
            }
            s_secure = true;
            NN_LOG_INF("tcp connected to %s:%u — streaming (encrypted)", s_host, s_port);
        } else {
            NN_LOG_WRN("tcp connected to %s:%u — streaming PLAINTEXT (no stream key)", s_host, s_port);
        }
        s_tcp_up = true;
        s_conn_fails = 0; s_reassoc_rounds = 0;   /* healthy again */
        xStreamBufferReset(s_sbuf);

        for (;;) {
            size_t n = xStreamBufferReceive(s_sbuf, chunk, sizeof chunk, pdMS_TO_TICKS(1000));
            if (n == 0) { if (!s_have_ip) break; continue; }
            if (s_secure) {
                if (nn_sectun_send(&s_tun, chunk, n) != ESP_OK) goto disc;
                s_tx_bytes += n;
            } else {
                size_t off = 0;
                while (off < n) {
                    int w = send(s_sock, chunk + off, n - off, 0);
                    if (w <= 0) goto disc;
                    off += w;
                    s_tx_bytes += w;
                }
            }
        }
    disc:
        NN_LOG_WRN("tcp send failed, reconnecting");
        s_tcp_up = false;
        close(s_sock); s_sock = -1;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void nn_netstream_send(const uint8_t *frame, size_t len)
{
    if (!s_tcp_up || !s_sbuf || len == 0) { s_tx_drops++; return; }
    /* All-or-nothing: only enqueue if the whole frame fits, so we never split
     * a frame across a drop (which would corrupt the H.264 stream). */
    if (xStreamBufferSpacesAvailable(s_sbuf) < len) { s_tx_drops++; return; }
    xStreamBufferSend(s_sbuf, frame, len, 0);
}

/* FRAME-ATOMIC drop for VIDEO: dropping a single fragment mid-frame truncates
 * the H.264 frame → the host decoder produces macroblock garbage that then
 * propagates through the whole GOP.  Once we have to drop any fragment of a
 * frame, drop ALL remaining fragments of that frame (until the next START) so
 * the frame is cleanly absent instead of corrupt.  NN_REC_VIDEO=0x56,
 * NN_VID_FLAG_START=0x02, NN_VID_FLAG_END=0x04. */
#define NNS_REC_VIDEO   0x56u
#define NNS_VID_START   0x02u
static bool s_vid_poison;

static SemaphoreHandle_t s_tx_mtx;   /* serialize send_record writers */

void nn_netstream_send_record(uint8_t type, uint8_t flags, uint16_t seq,
                              uint64_t ts_ms, const uint8_t *payload, size_t len)
{
    if (!s_tcp_up || !s_sbuf) { s_tx_drops++; s_drop_notcp++; return; }
    /* FreeRTOS stream buffers are SINGLE-writer, but the camera and audio
     * tasks both send records.  Without this lock an audio record interleaves
     * mid-video-payload and the host record parser desyncs permanently. */
    if (!s_tx_mtx || xSemaphoreTake(s_tx_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
        s_tx_drops++; return;
    }
    bool is_video = (type == NNS_REC_VIDEO);
    if (is_video && (flags & NNS_VID_START)) s_vid_poison = false;  /* new frame */
    if (is_video && s_vid_poison) { s_tx_drops++; xSemaphoreGive(s_tx_mtx); return; }
    size_t total = NN_REC_HDR_LEN + len;
    /* Header + payload must enter the byte-stream contiguously (single writer),
     * so the host can parse the record by its length prefix.  All-or-nothing. */
    if (xStreamBufferSpacesAvailable(s_sbuf) < total) {
        s_tx_drops++; s_drop_nospace++;
        if (is_video) s_vid_poison = true;   /* poison the rest of this frame */
        xSemaphoreGive(s_tx_mtx);
        return;
    }
    s_rec_writes++;
    uint8_t h[NN_REC_HDR_LEN];
    h[0] = type; h[1] = flags;
    h[2] = (uint8_t)(seq & 0xFF); h[3] = (uint8_t)(seq >> 8);
    for (int i = 0; i < 8; i++) h[4 + i] = (uint8_t)((ts_ms >> (8 * i)) & 0xFF);   /* u64 LE */
    h[12] = (uint8_t)(len & 0xFF);          h[13] = (uint8_t)((len >> 8) & 0xFF);
    h[14] = (uint8_t)((len >> 16) & 0xFF);  h[15] = (uint8_t)((len >> 24) & 0xFF);
    xStreamBufferSend(s_sbuf, h, NN_REC_HDR_LEN, 0);
    if (len) xStreamBufferSend(s_sbuf, payload, len, 0);
    xSemaphoreGive(s_tx_mtx);
}

void nn_netstream_status(char *out, size_t n)
{
    snprintf(out, n, "wifi=%s ip=%s tcp=%s enc=%s host=%s:%u tx=%lukB writes=%lu drops=%lu(notcp=%lu nospace=%lu)",
             s_ssid[0] ? s_ssid : "(unset)", s_have_ip ? "yes" : "no",
             s_tcp_up ? "up" : "down",
             s_have_stream_key ? (s_secure ? "on" : "pending") : "off",
             s_host, s_port,
             (unsigned long)(s_tx_bytes / 1024), (unsigned long)s_rec_writes,
             (unsigned long)s_tx_drops, (unsigned long)s_drop_notcp, (unsigned long)s_drop_nospace);
}

static StaticStreamBuffer_t s_sbuf_struct;
/* Static stream-buffer storage — reserved in internal BSS at LINK time
 * (deterministic, no heap use/fragmentation; an over-large size fails the BUILD,
 * not the runtime — which would have caught the old 512 KB NO_MEM immediately). */
static uint8_t s_sbuf_storage[SBUF_SIZE + 1];

/* ── Server->device control back-channel ─────────────────────────────────
 * The server encrypts control messages with the s2c key over the SAME sectun
 * session; nn_sectun_recv (k_rx/ctr_rx) decrypts them.  A dedicated task blocks
 * on recv so it never stalls the send drain (full-duplex TCP; send uses tx
 * state, recv uses rx state — disjoint).  Only runs while the session is up. */
static nn_netstream_ctrl_cb_t s_ctrl_cb;
static void                  *s_ctrl_ctx;

void nn_netstream_set_control_cb(nn_netstream_ctrl_cb_t cb, void *ctx)
{
    s_ctrl_cb = cb; s_ctrl_ctx = ctx;
}

static void ctrl_task(void *arg)
{
    (void)arg;
    static uint8_t msg[256];
    for (;;) {
        if (!s_tcp_up || !s_secure) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        size_t n = 0;
        /* Blocks until a control record arrives or the socket errors (on a
         * disconnect read_all fails → we loop and wait for the next session). */
        if (nn_sectun_recv(&s_tun, msg, sizeof msg, &n) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (n && s_ctrl_cb) s_ctrl_cb(msg, n, s_ctrl_ctx);
    }
}

esp_err_t nn_netstream_init(void)
{
    cfg_load();
    /* Static storage (s_sbuf_storage) — no runtime allocation. */
    s_sbuf = xStreamBufferCreateStatic(SBUF_SIZE, 1, s_sbuf_storage, &s_sbuf_struct);
    if (!s_sbuf) return ESP_ERR_NO_MEM;
    s_tx_mtx = xSemaphoreCreateMutex();
    if (!s_tx_mtx) return ESP_ERR_NO_MEM;
    NN_LOG_INF("stream buffer: %u KB (static BSS)", (unsigned)NNS_SBUF_KB);
    return ESP_OK;
}

esp_err_t nn_netstream_start(void)
{
    static bool started;
    if (started) return ESP_OK;
    esp_err_t ret = wifi_start();
    if (ret != ESP_OK) return ret;        /* e.g. no creds yet */
    /* 8 KB: net_task holds a TCP chunk + nn_sectun's ~4 KB record cipher buffer
     * on its stack during encrypted sends. */
    if (xTaskCreate(net_task, "nn_net", 8192, NULL, 8, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    /* Control receiver: only meaningful once a cb is registered, but harmless
     * otherwise (it just drops decrypted messages).  8 KB for the sectun recv
     * cipher buffer on its stack, matching net_task. */
    if (xTaskCreate(ctrl_task, "nn_ctrl", 8192, NULL, 7, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    started = true;
    return ESP_OK;
}
