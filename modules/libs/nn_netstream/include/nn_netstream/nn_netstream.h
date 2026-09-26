/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * nn_netstream — ESP32-C6 side: join Wi-Fi and push the H.264 video stream to
 * a host over TCP (the host runs a GStreamer `tcpserversrc` receiver).
 *
 *   ... -> C6 reassembles H.264 frame -> nn_netstream_send() -> Wi-Fi/TCP -> host
 *
 * Credentials + host are stored in NVS and settable at runtime via the `net`
 * console command, so nothing secret lives in the firmware image.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Init NVS-backed config + Wi-Fi/TCP machinery (does not connect yet). */
esp_err_t nn_netstream_init(void);

/* Persist Wi-Fi credentials / host endpoint (NVS). */
esp_err_t nn_netstream_set_wifi(const char *ssid, const char *pass);
esp_err_t nn_netstream_set_host(const char *ip, uint16_t port);

/* Provide the streaming service's X25519 public key.  When set (provisioned),
 * the uplink runs through an encrypted nn_sectun session instead of plaintext.
 * Pushed in by nn_prov so nn_netstream needn't depend on it. */
void nn_netstream_set_stream_key(const uint8_t pub[32]);

/* Connect Wi-Fi (STA) and start the TCP sender task.  Reconnects on drop. */
esp_err_t nn_netstream_start(void);

/* Enqueue one complete H.264 (Annex-B) frame for transmission.  Non-blocking;
 * drops the frame if Wi-Fi/TCP isn't up or the send buffer is full. */
void nn_netstream_send(const uint8_t *frame, size_t len);

/*
 * Typed uplink record (replaces the raw byte-stream when A/V are muxed).  Each
 * record is framed in-band over the (encrypted) TCP stream as:
 *
 *   [u8 type][u8 flags][u16 seq][u64 ts_ms LE][u32 len LE][payload ... len]
 *
 * type: NN_REC_VIDEO ('V') — payload is an H.264 (Annex-B) fragment;
 *       NN_REC_AUDIO ('A') — payload is one AAC (ADTS) frame.
 * ts_ms is ABSOLUTE hub-epoch milliseconds (uint64).  The host parses records
 * by the length prefix, routes video payloads to its H.264 sink and audio to
 * its AAC sink, and does the A/V sync/reorder by ts_ms (the C6 relays directly,
 * in arrival order — the host orders by timestamp).
 */
#define NN_REC_VIDEO   0x56u   /* 'V' */
#define NN_REC_AUDIO   0x41u   /* 'A' */
#define NN_REC_HDR_LEN 16u

/* Frame + enqueue one typed record (header + payload sent atomically).
 * Non-blocking; drops if the link is down or the whole record doesn't fit. */
void nn_netstream_send_record(uint8_t type, uint8_t flags, uint16_t seq,
                             uint64_t ts_ms, const uint8_t *payload, size_t len);

/* True once Wi-Fi has an IP and the TCP socket is connected. */
bool nn_netstream_is_streaming(void);

/* Monotonic count of records dropped because the send buffer was full
 * ("nospace") — the most immediate signal that the link can't keep up with the
 * encoder.  The adaptive controller polls the delta to drive bitrate/GOP. */
uint32_t nn_netstream_get_nospace_drops(void);

/* Server->device control back-channel: the callback is invoked (from an internal
 * task) with each decrypted control message the server sends over the same
 * encrypted session.  Register before nn_netstream_start(). */
typedef void (*nn_netstream_ctrl_cb_t)(const uint8_t *msg, size_t len, void *ctx);
void nn_netstream_set_control_cb(nn_netstream_ctrl_cb_t cb, void *ctx);

/* One-line status into `out`. */
void nn_netstream_status(char *out, size_t n);

/* Register the `net` console command. */
void nn_netstream_cli_register(void);

#ifdef __cplusplus
}
#endif
