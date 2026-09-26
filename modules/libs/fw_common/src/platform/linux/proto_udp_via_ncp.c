/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Linux implementation of the proto_udp.h API that bypasses the kernel
 * network stack entirely.  Outbound nn_proto frames are wrapped in
 * IPv6 + UDP headers built in user space and pushed via
 * ncp_link_set_raw(SPINEL_PROP_STREAM_NET).  Inbound STREAM_NET packets
 * arrive via the ncp_link STREAM_NET callback; we parse the IPv6 +
 * UDP headers ourselves and dispatch to the registered on_rx.
 *
 * No TUN device, no kernel routing table, no root privileges.  The
 * gateway daemon and the Thread mesh share nothing with the host
 * kernel other than the UART /dev/ttyAMA3.
 *
 * Gateway source IPv6 is read from the NCP's mesh-local EID
 * (SPINEL_PROP_IPV6_ML_ADDR) at init time.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <fw_common/log.h>
#include <fw_common/ncp_link.h>
#include <fw_common/proto_udp.h>
#include <fw_common/spinel.h>

LOG_MODULE_REGISTER(proto_udp, LOG_LEVEL_INF);

/* ── module state ─────────────────────────────────────────────────── */

static struct {
	struct proto_udp_config cfg;
	struct proto_udp_stats  stats;
	pthread_mutex_t         stats_lock;
	struct in6_addr         src_ml_eid;        /* gateway's mesh-local EID */
	bool                    src_ml_valid;
} S;

/* Thread realm-local mcast: ff03::1 */
static const struct in6_addr s_mcast_dst = {
	.s6_addr = { 0xff, 0x03, 0, 0, 0, 0, 0, 0,
		     0,    0,    0, 0, 0, 0, 0, 0x01 },
};

/* ── IPv6 + UDP packet build/parse ────────────────────────────────── */

static uint16_t rfc1071_checksum(const uint8_t *data, size_t len, uint32_t seed)
{
	uint32_t sum = seed;
	while (len >= 2) {
		sum += ((uint16_t)data[0] << 8) | data[1];
		data += 2; len -= 2;
	}
	if (len) sum += ((uint16_t)data[0] << 8);
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

static uint16_t udp6_checksum(const struct in6_addr *src,
			      const struct in6_addr *dst,
			      uint16_t src_port, uint16_t dst_port,
			      const uint8_t *payload, size_t payload_len)
{
	/* Pseudo-header (40 bytes) + UDP header (8 bytes) + payload.
	 * Fold via two-stage RFC 1071. */
	uint32_t sum = 0;
	/* src + dst */
	for (int i = 0; i < 16; i += 2) {
		sum += ((uint16_t)src->s6_addr[i] << 8) | src->s6_addr[i + 1];
	}
	for (int i = 0; i < 16; i += 2) {
		sum += ((uint16_t)dst->s6_addr[i] << 8) | dst->s6_addr[i + 1];
	}
	uint32_t udp_len = 8u + (uint32_t)payload_len;
	sum += (udp_len >> 16) & 0xffff;
	sum += udp_len         & 0xffff;
	sum += 17;  /* next-header = UDP */

	/* UDP header words: src_port, dst_port, length, checksum=0 */
	sum += src_port;
	sum += dst_port;
	sum += udp_len & 0xffff;
	/* checksum field = 0 during compute */

	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	uint16_t pseudo = (uint16_t)sum;

	uint16_t pl = rfc1071_checksum(payload, payload_len, 0);
	uint32_t combined = (uint32_t)pseudo + (uint32_t)(~pl & 0xffff);
	while (combined >> 16) combined = (combined & 0xffff) + (combined >> 16);
	uint16_t result = (uint16_t)~combined;
	return result == 0 ? 0xffff : result;
}

/* Build [u16 LE len][IPv6 header][UDP header][payload] into out_buf.
 * Returns total byte count or negative errno. */
static int build_stream_net(uint8_t *out_buf, size_t out_cap,
			    const struct in6_addr *src,
			    const struct in6_addr *dst,
			    uint16_t src_port, uint16_t dst_port,
			    const uint8_t *payload, size_t payload_len,
			    uint8_t hop_limit)
{
	const size_t ipv6_hdr = 40;
	const size_t udp_hdr  = 8;
	size_t pkt_len = ipv6_hdr + udp_hdr + payload_len;
	size_t total   = 2 + pkt_len;
	if (total > out_cap || pkt_len > 0xffff || payload_len > 0xffff - udp_hdr) {
		return -EMSGSIZE;
	}

	/* Spinel STREAM_NET prefix: u16 LE packet length, then the IPv6 packet. */
	out_buf[0] = (uint8_t)(pkt_len & 0xff);
	out_buf[1] = (uint8_t)(pkt_len >> 8);
	uint8_t *p = out_buf + 2;

	/* IPv6 header. */
	uint16_t udp_len = (uint16_t)(udp_hdr + payload_len);
	p[0] = 0x60;                          /* version=6, TC[high]=0 */
	p[1] = 0x00;                          /* TC[low]=0, flow-label=0 */
	p[2] = 0x00;
	p[3] = 0x00;
	p[4] = (uint8_t)((udp_len >> 8) & 0xff);  /* payload length */
	p[5] = (uint8_t)(udp_len & 0xff);
	p[6] = 17;                            /* next header = UDP */
	p[7] = hop_limit;
	memcpy(p + 8,  src, 16);
	memcpy(p + 24, dst, 16);
	p += ipv6_hdr;

	/* UDP header. */
	p[0] = (uint8_t)((src_port >> 8) & 0xff);
	p[1] = (uint8_t)(src_port & 0xff);
	p[2] = (uint8_t)((dst_port >> 8) & 0xff);
	p[3] = (uint8_t)(dst_port & 0xff);
	p[4] = (uint8_t)((udp_len >> 8) & 0xff);
	p[5] = (uint8_t)(udp_len & 0xff);
	/* Checksum: fill in below. */
	p[6] = 0; p[7] = 0;
	memcpy(p + udp_hdr, payload, payload_len);

	uint16_t csum = udp6_checksum(src, dst, src_port, dst_port,
				      payload, payload_len);
	p[6] = (uint8_t)((csum >> 8) & 0xff);
	p[7] = (uint8_t)(csum & 0xff);

	return (int)total;
}

/* ── inbound STREAM_NET callback ─────────────────────────────────── */

static void on_stream_net(const uint8_t *value, size_t value_len)
{
	/* STREAM_NET incoming value: u16 LE length + IPv6 packet (no metadata
	 * trail for our purposes). */
	if (value_len < 2 + 40 + 8) {
		pthread_mutex_lock(&S.stats_lock);
		S.stats.rx_drops_bad++;
		pthread_mutex_unlock(&S.stats_lock);
		return;
	}
	uint16_t pkt_len = (uint16_t)value[0] | ((uint16_t)value[1] << 8);
	if ((size_t)pkt_len + 2 > value_len) {
		pthread_mutex_lock(&S.stats_lock);
		S.stats.rx_drops_bad++;
		pthread_mutex_unlock(&S.stats_lock);
		return;
	}
	const uint8_t *pkt = value + 2;
	if ((pkt[0] >> 4) != 6) return;          /* not IPv6 */
	uint8_t  next_hdr   = pkt[6];
	if (next_hdr != 17) return;              /* not UDP */
	uint16_t ipv6_payload_len = ((uint16_t)pkt[4] << 8) | pkt[5];
	if ((size_t)40 + ipv6_payload_len > pkt_len) return;

	const uint8_t *udp = pkt + 40;
	uint16_t src_port = ((uint16_t)udp[0] << 8) | udp[1];
	uint16_t dst_port = ((uint16_t)udp[2] << 8) | udp[3];
	uint16_t udp_len  = ((uint16_t)udp[4] << 8) | udp[5];
	if (udp_len < 8 || (size_t)udp_len != (size_t)ipv6_payload_len) return;
	if (dst_port != S.cfg.port) return;

	const uint8_t *payload = udp + 8;
	size_t         payload_len = (size_t)udp_len - 8;

	pthread_mutex_lock(&S.stats_lock);
	S.stats.rx_frames++;
	S.stats.rx_bytes += (uint32_t)payload_len;
	pthread_mutex_unlock(&S.stats_lock);

	if (S.cfg.on_rx) {
		struct in6_addr src;
		memcpy(&src, pkt + 8, 16);
		S.cfg.on_rx(payload, payload_len, &src, src_port,
			    S.cfg.on_rx_user);
	}
}

/* ── public API ───────────────────────────────────────────────────── */

int proto_udp_init(const struct proto_udp_config *cfg)
{
	if (!cfg || !cfg->port) return -EINVAL;
	S.cfg = *cfg;
	pthread_mutex_init(&S.stats_lock, NULL);

	/* Pull the gateway's mesh-local EID from the NCP so outbound
	 * packets carry a valid Thread-side source IPv6.  The NCP needs
	 * to have come up and joined the Thread network before this
	 * resolves to a non-zero address. */
	uint8_t buf[64];
	size_t  len = sizeof buf;
	int rv = ncp_link_get(SPINEL_PROP_IPV6_ML_ADDR, buf, &len, 1000);
	if (rv == 0 && len >= 16) {
		memcpy(&S.src_ml_eid, buf, 16);
		S.src_ml_valid = true;
		char buf2[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &S.src_ml_eid, buf2, sizeof buf2);
		LOG_INF("ML-EID: %s", buf2);
	} else {
		LOG_WRN("ML-EID query failed (rv=%d len=%zu); outbound will "
			"use :: until Thread up", rv, len);
	}

	ncp_link_set_stream_net_cb(on_stream_net);
	LOG_INF("proto_udp via ncp_link: bound port=%u", cfg->port);
	return 0;
}

static int send_via_stream_net(const struct in6_addr *dst, uint16_t dst_port,
			       const uint8_t *frame, size_t len, bool mcast)
{
	if (!S.src_ml_valid) {
		LOG_WRN("send rejected: no ML-EID yet (Thread not up?)");
		return -ENETDOWN;
	}
	static uint8_t wire[2 + 40 + 8 + 1280];
	int built = build_stream_net(wire, sizeof wire,
				     &S.src_ml_eid, dst,
				     S.cfg.port, dst_port,
				     frame, len,
				     /*hop_limit=*/ mcast ? 1 : 64);
	if (built < 0) {
		pthread_mutex_lock(&S.stats_lock);
		S.stats.tx_failures++;
		pthread_mutex_unlock(&S.stats_lock);
		return built;
	}
	uint32_t last_status = 0;
	int rv = ncp_link_set_raw(SPINEL_PROP_STREAM_NET,
				  wire, (size_t)built, &last_status, 500);
	if (rv < 0) {
		LOG_WRN("STREAM_NET set rv=%d last_status=%u", rv, last_status);
		pthread_mutex_lock(&S.stats_lock);
		S.stats.tx_failures++;
		pthread_mutex_unlock(&S.stats_lock);
		return rv;
	}
	pthread_mutex_lock(&S.stats_lock);
	if (mcast) S.stats.tx_mcast++;
	else       S.stats.tx_unicast++;
	pthread_mutex_unlock(&S.stats_lock);
	return 0;
}

int proto_udp_send_unicast(const struct in6_addr *dst, uint16_t dst_port,
			   const uint8_t *frame, size_t len)
{
	if (!dst || !frame || len == 0) return -EINVAL;
	return send_via_stream_net(dst, dst_port, frame, len, false);
}

int proto_udp_send_mcast(const uint8_t *frame, size_t len)
{
	if (!frame || len == 0) return -EINVAL;
	return send_via_stream_net(&s_mcast_dst, S.cfg.port, frame, len, true);
}

void proto_udp_get_stats(struct proto_udp_stats *out)
{
	if (!out) return;
	pthread_mutex_lock(&S.stats_lock);
	*out = S.stats;
	pthread_mutex_unlock(&S.stats_lock);
}
