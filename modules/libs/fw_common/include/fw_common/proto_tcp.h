/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto_tcp — gateway-side TCP transport for the nn_proto wire format.
 *
 * Maintains a single TCP connection to the hub, resolves it via mDNS
 * (or accepts a literal address), and exposes a frame-in / frame-out
 * interface for nn_proto traffic.  Identical API on both Zephyr and
 * Linux backends; the platform-specific implementation lives under
 * src/platform/{zephyr,linux}/proto_tcp.c.
 *
 * Behaviour
 *   - One TCP connection at a time; exponential reconnect backoff.
 *   - Single worker thread (Zephyr k_thread or pthread on Linux).
 *   - TX path: caller enqueues a complete nn_proto frame; the worker
 *     drains a ring/queue to the socket.
 *   - RX path: the worker reads bytes, accumulates into an internal
 *     buffer, and dispatches every complete frame to the registered
 *     on_rx callback (called FROM the worker thread).
 */

#ifndef FW_COMMON_PROTO_TCP_H_
#define FW_COMMON_PROTO_TCP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum proto_tcp_state {
	PROTO_TCP_DOWN = 0,
	PROTO_TCP_CONNECTING,
	PROTO_TCP_UP,
};

typedef void (*proto_tcp_rx_fn)(const uint8_t *buf, size_t len, void *user);

struct proto_tcp_config {
	const char     *hub_hostname;  /* mDNS hostname or IP literal */
	uint16_t        hub_port;
	proto_tcp_rx_fn on_rx;
	void           *on_rx_user;
};

int proto_tcp_init(const struct proto_tcp_config *cfg);
int proto_tcp_set_hub_hostname(const char *hostname);
int proto_tcp_enqueue(const uint8_t *frame, size_t len);
enum proto_tcp_state proto_tcp_get_state(void);

struct proto_tcp_stats {
	uint32_t connects;
	uint32_t connect_failures;
	uint32_t disconnects;
	uint32_t tx_frames;
	uint32_t tx_bytes;
	uint32_t tx_drops_full;
	uint32_t tx_drops_socket;
	uint32_t rx_frames;
	uint32_t rx_bytes;
	uint32_t rx_drops_oversize;
	uint32_t rx_drops_bad;
};

void proto_tcp_get_stats(struct proto_tcp_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* FW_COMMON_PROTO_TCP_H_ */
