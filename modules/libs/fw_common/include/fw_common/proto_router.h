/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto_router — gateway-side broker glue (platform-neutral).
 *
 * Plumbs:
 *   UDP (Thread side) RX → routing decision → TCP (hub side) TX
 *   TCP (hub side)    RX → routing decision → UDP (Thread side) TX
 *
 * For D2H/H2D the gateway forwards opaquely (no inner inspection, no
 * re-signing).  For D2G/G2D the gateway terminates / originates.
 *
 * Maintains a small device_id → mesh-local IPv6 routing table,
 * populated from incoming D2H/D2G source addresses.  Fixed-size LRU.
 */

#ifndef FW_COMMON_PROTO_ROUTER_H_
#define FW_COMMON_PROTO_ROUTER_H_

#include <netinet/in.h>  /* struct in6_addr — also present on Zephyr libc */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_ROUTER_DEVICE_ID_MAX  16
#ifndef PROTO_ROUTER_TABLE_SIZE
#define PROTO_ROUTER_TABLE_SIZE     32
#endif

int  proto_router_init(void);
int  proto_router_on_udp_rx(const uint8_t *frame, size_t len,
			    const struct in6_addr *src);
int  proto_router_on_tcp_rx(const uint8_t *frame, size_t len);

/* Hub-to-gateway (NN_PROTO_TYPE_H2G) frames are for the gateway itself:
 * proto_router hands them, unverified, to this handler (NULL = drop). */
typedef void (*proto_router_h2g_fn)(const uint8_t *frame, size_t len);
void proto_router_set_h2g_handler(proto_router_h2g_fn fn);

/* Called when an H2D frame from the hub cannot be forwarded (device not in
 * the routing table: -EHOSTUNREACH; UDP send failed: -EIO) so the gateway
 * can tell the hub, which re-sends it via another gateway. */
typedef void (*proto_router_h2d_fail_fn)(const uint8_t *device_id, uint16_t did_size,
					 const uint8_t *payload, size_t payload_len, int reason);
void proto_router_set_h2d_fail_handler(proto_router_h2d_fail_fn fn);

int  proto_router_remember(const uint8_t *device_id, uint16_t did_size,
			   const struct in6_addr *addr);
int  proto_router_lookup(const uint8_t *device_id, uint16_t did_size,
			 struct in6_addr *out_addr);

struct proto_router_entry {
	uint8_t          device_id[PROTO_ROUTER_DEVICE_ID_MAX];
	uint16_t         did_size;
	struct in6_addr  addr;
};

/* Copies up to `max` cached entries into `out` under the router lock.
 * Returns the number of entries copied (≤ max), or negative errno. */
int  proto_router_snapshot(struct proto_router_entry *out, int max);

struct proto_router_stats {
	uint32_t in_d2h;
	uint32_t in_h2d;
	uint32_t in_d2g;
	uint32_t in_g2d;
	uint32_t fwd_d2h_to_tcp;
	uint32_t fwd_h2d_to_udp;
	uint32_t drops_unknown_did;
	uint32_t drops_tcp_enqueue;
	uint32_t drops_udp_send;
	uint32_t drops_bad_type;
	uint32_t table_inserts;
	uint32_t table_evictions;
};

void proto_router_get_stats(struct proto_router_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* FW_COMMON_PROTO_ROUTER_H_ */
