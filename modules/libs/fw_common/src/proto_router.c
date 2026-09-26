/* SPDX-License-Identifier: Apache-2.0 */

/*
 * proto_router — platform-neutral nn_proto frame router.  Bridges
 * UDP (Thread-side) RX and TCP (hub-side) RX paths, learning device
 * IPv6 addresses from the source of every inbound D2H/D2G frame.
 *
 * Locking: pthread_mutex on both Linux and Zephyr (the Zephyr libc
 * provides a pthread shim sufficient for our purposes).  Time stamps
 * use clock_gettime(CLOCK_MONOTONIC), also platform-neutral.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <time.h>

#include <nn_proto/nn_proto.h>

#include <fw_common/log.h>
#include <fw_common/proto_router.h>
#include <fw_common/proto_tcp.h>
#include <fw_common/proto_udp.h>

LOG_MODULE_REGISTER(proto_router, LOG_LEVEL_INF);

#ifndef PROTO_ROUTER_UDP_PORT
#define PROTO_ROUTER_UDP_PORT 49190   /* matches CONFIG_NODE_MGR_NN_PROTO_PORT */
#endif

/* ── routing table ────────────────────────────────────────────────── */

struct route_entry {
	uint8_t          device_id[PROTO_ROUTER_DEVICE_ID_MAX];
	uint16_t         did_size;
	struct in6_addr  addr;
	uint64_t         last_seen_ms;
	bool             used;
};

static struct {
	struct route_entry         table[PROTO_ROUTER_TABLE_SIZE];
	pthread_mutex_t            lock;
	struct proto_router_stats  stats;
} R;

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int find_entry(const uint8_t *did, uint16_t did_size)
{
	for (int i = 0; i < PROTO_ROUTER_TABLE_SIZE; i++) {
		if (R.table[i].used &&
		    R.table[i].did_size == did_size &&
		    memcmp(R.table[i].device_id, did, did_size) == 0) {
			return i;
		}
	}
	return -1;
}

static int find_lru_or_free(void)
{
	int best = -1;
	uint64_t oldest = UINT64_MAX;
	for (int i = 0; i < PROTO_ROUTER_TABLE_SIZE; i++) {
		if (!R.table[i].used) return i;
		if (R.table[i].last_seen_ms < oldest) {
			oldest = R.table[i].last_seen_ms;
			best = i;
		}
	}
	return best;
}

int proto_router_remember(const uint8_t *device_id, uint16_t did_size,
			  const struct in6_addr *addr)
{
	if (!device_id || !addr || did_size == 0 ||
	    did_size > PROTO_ROUTER_DEVICE_ID_MAX) {
		return -EINVAL;
	}
	pthread_mutex_lock(&R.lock);
	int idx = find_entry(device_id, did_size);
	if (idx < 0) {
		idx = find_lru_or_free();
		if (R.table[idx].used)  R.stats.table_evictions++;
		else                    R.stats.table_inserts++;
	}
	memset(&R.table[idx], 0, sizeof R.table[idx]);
	memcpy(R.table[idx].device_id, device_id, did_size);
	R.table[idx].did_size     = did_size;
	memcpy(&R.table[idx].addr, addr, sizeof *addr);
	R.table[idx].last_seen_ms = now_ms();
	R.table[idx].used         = true;
	pthread_mutex_unlock(&R.lock);
	return 0;
}

int proto_router_lookup(const uint8_t *device_id, uint16_t did_size,
			struct in6_addr *out_addr)
{
	if (!device_id || did_size == 0 || did_size > PROTO_ROUTER_DEVICE_ID_MAX) {
		return -EINVAL;
	}
	pthread_mutex_lock(&R.lock);
	int idx = find_entry(device_id, did_size);
	if (idx < 0) {
		pthread_mutex_unlock(&R.lock);
		return -ENOENT;
	}
	if (out_addr) memcpy(out_addr, &R.table[idx].addr, sizeof *out_addr);
	R.table[idx].last_seen_ms = now_ms();
	pthread_mutex_unlock(&R.lock);
	return 0;
}

void proto_router_get_stats(struct proto_router_stats *out)
{
	if (out) *out = R.stats;
}

int proto_router_snapshot(struct proto_router_entry *out, int max)
{
	if (!out || max <= 0) return -EINVAL;
	int n = 0;
	pthread_mutex_lock(&R.lock);
	for (int i = 0; i < PROTO_ROUTER_TABLE_SIZE && n < max; i++) {
		if (!R.table[i].used) continue;
		memcpy(out[n].device_id, R.table[i].device_id, PROTO_ROUTER_DEVICE_ID_MAX);
		out[n].did_size = R.table[i].did_size;
		memcpy(&out[n].addr, &R.table[i].addr, sizeof out[n].addr);
		n++;
	}
	pthread_mutex_unlock(&R.lock);
	return n;
}

/* ── dispatch ─────────────────────────────────────────────────────── */

int proto_router_on_udp_rx(const uint8_t *frame, size_t len,
			   const struct in6_addr *src)
{
	struct nn_proto_view view;
	int rv = nn_proto_parse(frame, len, &view, NULL);
	if (rv != 0) {
		R.stats.drops_bad_type++;
		return rv;
	}

	if (view.device_id_size > 0 &&
	    view.device_id_size <= PROTO_ROUTER_DEVICE_ID_MAX) {
		(void)proto_router_remember(view.device_id,
					    view.device_id_size, src);
	}

	switch (view.type) {
	case NN_PROTO_TYPE_D2H:
		R.stats.in_d2h++;
		if (proto_tcp_enqueue(frame, len) < 0) {
			R.stats.drops_tcp_enqueue++;
			return -ENOMEM;
		}
		R.stats.fwd_d2h_to_tcp++;
		return 0;

	case NN_PROTO_TYPE_D2G:
		R.stats.in_d2g++;
		LOG_DBG("D2G received (dropped — handler not wired)");
		return 0;

	default:
		R.stats.drops_bad_type++;
		LOG_WRN("unexpected type 0x%04x on UDP", view.type);
		return -EBADF;
	}
}

static proto_router_h2g_fn g_h2g_fn;

void proto_router_set_h2g_handler(proto_router_h2g_fn fn) { g_h2g_fn = fn; }

static proto_router_h2d_fail_fn g_h2d_fail_fn;

void proto_router_set_h2d_fail_handler(proto_router_h2d_fail_fn fn) { g_h2d_fail_fn = fn; }

int proto_router_on_tcp_rx(const uint8_t *frame, size_t len)
{
	struct nn_proto_view view;
	int rv = nn_proto_parse(frame, len, &view, NULL);
	if (rv != 0) {
		R.stats.drops_bad_type++;
		return rv;
	}

	struct in6_addr dst;
	switch (view.type) {
	case NN_PROTO_TYPE_H2D:
		R.stats.in_h2d++;
		if (view.device_id_size == 0) {
			R.stats.drops_unknown_did++;
			return -EINVAL;
		}
		if (proto_router_lookup(view.device_id, view.device_id_size,
					&dst) < 0) {
			R.stats.drops_unknown_did++;
			LOG_WRN("H2D: device_id not in routing table");
			if (g_h2d_fail_fn)
				g_h2d_fail_fn(view.device_id, view.device_id_size,
					      view.payload, view.payload_size, -EHOSTUNREACH);
			return -EHOSTUNREACH;
		}
		{
			int urv = proto_udp_send_unicast(&dst, PROTO_ROUTER_UDP_PORT,
							 frame, len);
			/* One line per hub->device frame: the exact address it
			 * went to (the device's source address from its last
			 * D2H), so a lost ack can be traced to where it was sent. */
			char a[INET6_ADDRSTRLEN] = "?";
			inet_ntop(AF_INET6, &dst, a, sizeof a);
			uint16_t icmd = 0; uint32_t itid = 0;
			if (view.payload_size >= 6) {
				icmd = (uint16_t)(view.payload[0] | (view.payload[1] << 8));
				itid = (uint32_t)view.payload[2] | ((uint32_t)view.payload[3] << 8) |
				       ((uint32_t)view.payload[4] << 16) | ((uint32_t)view.payload[5] << 24);
			}
			LOG_INF("H2D cmd=0x%04x tid=%u -> %s rv=%d", icmd, itid, a, urv);
			if (urv < 0) {
				R.stats.drops_udp_send++;
				if (g_h2d_fail_fn)
					g_h2d_fail_fn(view.device_id, view.device_id_size,
						      view.payload, view.payload_size, -EIO);
				return -EIO;
			}
		}
		R.stats.fwd_h2d_to_udp++;
		return 0;

	case NN_PROTO_TYPE_H2G:
		if (g_h2g_fn) {
			g_h2g_fn(frame, len);
			return 0;
		}
		R.stats.drops_bad_type++;
		return -ENOTSUP;

	case NN_PROTO_TYPE_G2D:
		R.stats.in_g2d++;
		if (view.device_id_size == 0) {
			if (proto_udp_send_mcast(frame, len) < 0) {
				R.stats.drops_udp_send++;
				return -EIO;
			}
			return 0;
		}
		if (proto_router_lookup(view.device_id, view.device_id_size,
					&dst) < 0) {
			R.stats.drops_unknown_did++;
			return -EHOSTUNREACH;
		}
		if (proto_udp_send_unicast(&dst, PROTO_ROUTER_UDP_PORT,
					   frame, len) < 0) {
			R.stats.drops_udp_send++;
			return -EIO;
		}
		return 0;

	default:
		R.stats.drops_bad_type++;
		LOG_WRN("unexpected type 0x%04x on TCP", view.type);
		return -EBADF;
	}
}

int proto_router_init(void)
{
	pthread_mutex_init(&R.lock, NULL);
	memset(R.table, 0, sizeof R.table);
	return 0;
}
