/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto_udp — gateway-side UDP transport for the nn_proto wire format
 * over the Thread mesh.  Same public API on both platforms; backends
 * differ:
 *
 *   Zephyr backend: opens AF_INET6 SOCK_DGRAM on the Thread net_if
 *                   that ncp_netif registered; uses Zephyr's network
 *                   stack for routing.  Lives under apps/ncp_host_esp32c6/.
 *
 *   Linux backend:  bypasses the kernel netstack entirely.  Sending
 *                   builds IPv6 + UDP headers in user space and forwards
 *                   via ncp_link_set_raw(SPINEL_PROP_STREAM_NET, …).
 *                   Receiving registers the STREAM_NET callback on
 *                   ncp_link, parses the IPv6/UDP headers, dispatches to
 *                   on_rx.  No TUN, no kernel routes, no root.  Lives
 *                   under src/platform/linux/proto_udp_via_ncp.c.
 */

#ifndef FW_COMMON_PROTO_UDP_H_
#define FW_COMMON_PROTO_UDP_H_

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*proto_udp_rx_fn)(const uint8_t *frame, size_t len,
				const struct in6_addr *src,
				uint16_t src_port,
				void *user);

struct proto_udp_config {
	uint16_t        port;          /* UDP port to bind / forward to */
	proto_udp_rx_fn on_rx;
	void           *on_rx_user;
};

int proto_udp_init(const struct proto_udp_config *cfg);

int proto_udp_send_unicast(const struct in6_addr *dst, uint16_t dst_port,
			   const uint8_t *frame, size_t len);

int proto_udp_send_mcast(const uint8_t *frame, size_t len);

struct proto_udp_stats {
	uint32_t rx_frames;
	uint32_t rx_bytes;
	uint32_t rx_drops_oversize;
	uint32_t rx_drops_bad;
	uint32_t tx_unicast;
	uint32_t tx_mcast;
	uint32_t tx_failures;
};

void proto_udp_get_stats(struct proto_udp_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* FW_COMMON_PROTO_UDP_H_ */
