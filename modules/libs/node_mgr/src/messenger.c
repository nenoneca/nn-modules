/* SPDX-License-Identifier: Apache-2.0 */

/*
 * messenger.c — UDP send/receive over Thread mesh with mDNS name resolution.
 *
 * Why custom mDNS queries instead of the platform DNS resolver:
 *   Zephyr's resolver stores the mDNS server as "ff02::fb" without a
 *   scope_id.  Sending to link-local multicast without a scope ID fails
 *   with EINVAL.  Instead we build and send our own DNS query packet on
 *   a UDP socket explicitly addressed via the Thread iface's scope_id,
 *   then parse the unicast reply (Zephyr's mDNS responder unicasts back
 *   when the query source port ≠ 5353).
 *
 * Send path:
 *   1. Check dns_cache for the target hostname.
 *   2. On miss (or stale): send a DNS AAAA query to ff02::fb:5353 via
 *      the Thread iface, wait up to MDNS_TIMEOUT_MS for response.
 *   3. Prefer mesh-local (fd::/8) over link-local (fe80::/10) in
 *      results.
 *   4. Cache the resolved address, then send a UDP datagram to port
 *      CONFIG_NODE_MGR_MESSENGER_PORT.
 *
 * Receive path:
 *   A dedicated thread binds the messenger UDP port on in6addr_any and
 *   prints every incoming datagram to the console.
 *
 * All socket / address operations go through nn_osal_socket — no
 * direct Zephyr net headers.
 */

#include <nn_osal/osal.h>
#include <nn_pal/openthread.h>
#include <node_mgr/messenger.h>
#include <node_mgr/dns_cache.h>

#include <string.h>
#include <errno.h>

NN_OSAL_LOG_MODULE(messenger);

#define MDNS_PORT          5353
#define MDNS_TIMEOUT_MS    3000
#define DNS_QTYPE_AAAA     28
#define DNS_CLASS_IN       1

/* ── helpers ──────────────────────────────────────────────────────── */

static bool addr_is_link_local(const uint8_t a[16])
{
	return a[0] == 0xfeu && (a[1] & 0xc0u) == 0x80u;
}

/* ── DNS wire-format helpers ──────────────────────────────────────── */

/* Encode a dotted-label hostname into DNS wire format.
 * E.g. "device2.local" → \x07device2\x05local\x00
 * Returns number of bytes written, or negative on error. */
static int encode_dns_name(uint8_t *dst, size_t dstlen, const char *name)
{
	size_t pos = 0;
	const char *p = name;

	while (*p) {
		const char *dot = strchr(p, '.');
		size_t label_len = dot ? (size_t)(dot - p) : strlen(p);

		if (label_len == 0 || label_len > 63) {
			return -EINVAL;
		}
		if (pos + 1 + label_len >= dstlen) {
			return -ENOMEM;
		}
		dst[pos++] = (uint8_t)label_len;
		memcpy(dst + pos, p, label_len);
		pos += label_len;
		p = dot ? dot + 1 : p + label_len;
	}

	if (pos + 1 > dstlen) return -ENOMEM;
	dst[pos++] = 0; /* root label */
	return (int)pos;
}

/* Build a DNS AAAA query packet.  Returns total packet length. */
static int build_dns_query(uint8_t *buf, size_t buflen, const char *hostname)
{
	if (buflen < 12) return -ENOMEM;

	/* DNS header: ID=0 (mDNS convention), FLAGS=0, QDCOUNT=1 */
	memset(buf, 0, 12);
	buf[5] = 1;

	int name_len = encode_dns_name(buf + 12, buflen - 12, hostname);
	if (name_len < 0) return name_len;

	size_t pos = 12 + name_len;
	if (pos + 4 > buflen) return -ENOMEM;

	buf[pos++] = 0x00;
	buf[pos++] = DNS_QTYPE_AAAA;
	buf[pos++] = 0x00;
	buf[pos++] = DNS_CLASS_IN;
	return (int)pos;
}

/* Skip one DNS name (handles pointer compression). */
static int skip_dns_name(const uint8_t *buf, size_t buflen, int pos)
{
	while (pos < (int)buflen) {
		uint8_t len = buf[pos];
		if (len == 0)            return pos + 1;
		if ((len & 0xc0) == 0xc0) return pos + 2;  /* compression ptr */
		pos += 1 + len;
	}
	return -EINVAL;
}

/* Parse a DNS response; return the first AAAA record's address. */
static int parse_dns_response(const uint8_t *buf, size_t len,
			      uint8_t out_addr[16])
{
	if (len < 12) return -EINVAL;
	uint16_t qdcount = ((uint16_t)buf[4] << 8) | buf[5];
	uint16_t ancount = ((uint16_t)buf[6] << 8) | buf[7];

	int pos = 12;
	for (int q = 0; q < qdcount; q++) {
		pos = skip_dns_name(buf, len, pos);
		if (pos < 0 || pos + 4 > (int)len) return -EINVAL;
		pos += 4;   /* QTYPE + QCLASS */
	}
	for (int a = 0; a < ancount; a++) {
		pos = skip_dns_name(buf, len, pos);
		if (pos < 0 || pos + 10 > (int)len) return -EINVAL;
		uint16_t rtype = ((uint16_t)buf[pos]   << 8) | buf[pos+1]; pos += 2;
		/* rclass */ pos += 2;
		/* TTL    */ pos += 4;
		uint16_t rdlen = ((uint16_t)buf[pos]   << 8) | buf[pos+1]; pos += 2;
		if (rtype == DNS_QTYPE_AAAA && rdlen == 16) {
			if (pos + 16 > (int)len) return -EINVAL;
			memcpy(out_addr, buf + pos, 16);
			return 0;
		}
		pos += rdlen;
		if (pos > (int)len) return -EINVAL;
	}
	return -ENOENT;
}

/* ── mDNS querier ─────────────────────────────────────────────────── */

static int mdns_query_aaaa(const char *hostname,
			   uint8_t out_addr[16], uint32_t *out_scope)
{
	uint32_t iface_idx = nn_pal_ot_iface_scope_id();
	if (iface_idx == 0) {
		NN_LOG_ERR("No OpenThread interface");
		return -ENODEV;
	}

	nn_osal_socket_t sock = nn_osal_socket(NN_OSAL_AF_INET6,
					       NN_OSAL_SOCK_DGRAM,
					       NN_OSAL_IPPROTO_UDP);
	if (sock < 0) {
		NN_LOG_ERR("socket() failed: %d", sock);
		return sock;
	}

	/* Bind ephemeral source port on in6addr_any. */
	nn_osal_sockaddr_in6_t bind_addr = { .port = 0, .scope_id = 0 };
	int rv = nn_osal_bind(sock, &bind_addr);
	if (rv < 0) {
		NN_LOG_ERR("bind() failed: %d", rv);
		nn_osal_close(sock);
		return rv;
	}

	/* Build the query. */
	uint8_t query[128];
	int query_len = build_dns_query(query, sizeof(query), hostname);
	if (query_len < 0) {
		NN_LOG_ERR("build_dns_query failed: %d", query_len);
		nn_osal_close(sock);
		return query_len;
	}

	/* Dst: ff02::fb:5353 on the Thread iface. */
	nn_osal_sockaddr_in6_t dst = { .port = MDNS_PORT, .scope_id = iface_idx };
	if (nn_osal_inet_pton6("ff02::fb", dst.addr) != 0) {
		nn_osal_close(sock);
		return -EINVAL;
	}

	NN_LOG_INF("mDNS AAAA query: %s (iface scope %u)", hostname, iface_idx);
	rv = nn_osal_sendto(sock, query, query_len, 0, &dst);
	if (rv < 0) {
		NN_LOG_ERR("sendto(ff02::fb) failed: %d", rv);
		nn_osal_close(sock);
		return rv;
	}

	uint8_t resp[512];
	uint8_t best[16] = {0}, fallback[16] = {0};
	bool got_best = false, got_fall = false;
	uint32_t deadline = nn_osal_uptime_ms_32() + MDNS_TIMEOUT_MS;

	while (true) {
		int32_t remaining_ms = (int32_t)(deadline - nn_osal_uptime_ms_32());
		if (remaining_ms <= 0) break;

		nn_osal_pollfd_t pfd = { .sock = sock, .events = NN_OSAL_POLLIN };
		int poll_ret = nn_osal_poll(&pfd, 1, remaining_ms);
		if (poll_ret <= 0) break;

		nn_osal_sockaddr_in6_t from;
		int n = nn_osal_recvfrom(sock, resp, sizeof(resp), 0, &from);
		if (n < 0) break;

		uint8_t addr[16];
		if (parse_dns_response(resp, (size_t)n, addr) != 0) continue;

		if (!addr_is_link_local(addr)) {
			memcpy(best, addr, 16);
			got_best = true;
			break;
		} else if (!got_fall) {
			memcpy(fallback, addr, 16);
			got_fall = true;
		}
	}

	nn_osal_close(sock);

	if (got_best) {
		memcpy(out_addr, best, 16);
		*out_scope = 0;
		return 0;
	}
	if (got_fall) {
		memcpy(out_addr, fallback, 16);
		*out_scope = iface_idx;
		return 0;
	}
	NN_LOG_WRN("mDNS: no AAAA response for %s", hostname);
	return -EHOSTUNREACH;
}

/* ── public: send ─────────────────────────────────────────────────── */

int messenger_send(const char *hostname, const char *message)
{
	uint8_t  addr[16];
	uint32_t scope_id = 0;
	int      rc;

	rc = dns_cache_lookup(hostname, addr, &scope_id);
	if (rc == -ESTALE) {
		dns_cache_remove(hostname);
		rc = -ENOENT;
	}
	if (rc != 0) {
		rc = mdns_query_aaaa(hostname, addr, &scope_id);
		if (rc != 0) return rc;
		dns_cache_insert(hostname, addr, scope_id);
	}

	nn_osal_socket_t sock = nn_osal_socket(NN_OSAL_AF_INET6,
					       NN_OSAL_SOCK_DGRAM,
					       NN_OSAL_IPPROTO_UDP);
	if (sock < 0) {
		NN_LOG_ERR("socket() failed: %d", sock);
		return sock;
	}

	nn_osal_sockaddr_in6_t dst = {
		.port     = CONFIG_NODE_MGR_MESSENGER_PORT,
		.scope_id = scope_id,
	};
	memcpy(dst.addr, addr, 16);

	int sent = nn_osal_sendto(sock, message, strlen(message), 0, &dst);
	nn_osal_close(sock);

	if (sent < 0) {
		NN_LOG_ERR("sendto(%s) failed: %d", hostname, sent);
		return sent;
	}

	char addr_str[NN_OSAL_INET6_ADDRSTRLEN];
	nn_osal_inet_ntop6(addr, addr_str, sizeof(addr_str));
	NN_LOG_INF("sent %d B → %s (%s)", sent, hostname, addr_str);
	return 0;
}

/* ── receive thread ───────────────────────────────────────────────── */

static void recv_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	nn_osal_socket_t sock = -1;
	while (sock < 0) {
		sock = nn_osal_socket(NN_OSAL_AF_INET6,
				      NN_OSAL_SOCK_DGRAM,
				      NN_OSAL_IPPROTO_UDP);
		if (sock < 0) {
			nn_osal_sleep_ms(1000);
			continue;
		}
		nn_osal_sockaddr_in6_t bind_addr = {
			.port = CONFIG_NODE_MGR_MESSENGER_PORT,
		};
		if (nn_osal_bind(sock, &bind_addr) < 0) {
			nn_osal_close(sock);
			sock = -1;
			nn_osal_sleep_ms(1000);
		}
	}

	NN_LOG_INF("recv thread listening on :%d", CONFIG_NODE_MGR_MESSENGER_PORT);

	char buf[CONFIG_NODE_MGR_MSG_MAX_LEN + 1];
	char addr_str[NN_OSAL_INET6_ADDRSTRLEN];

	while (1) {
		nn_osal_sockaddr_in6_t from;
		int n = nn_osal_recvfrom(sock, buf, CONFIG_NODE_MGR_MSG_MAX_LEN,
					 0, &from);
		if (n <= 0) continue;
		buf[n] = '\0';
		nn_osal_inet_ntop6(from.addr, addr_str, sizeof(addr_str));
		printk("\n\033[1;32m[mesh]\033[0m %s: %s\n", addr_str, buf);
	}
}

K_THREAD_DEFINE(recv_thread, CONFIG_NODE_MGR_RECV_STACK_SIZE,
		recv_thread_fn, NULL, NULL, NULL,
		K_PRIO_COOP(7), 0, 0);

void messenger_start(void)
{
	NN_LOG_INF("messenger ready (UDP port %d)", CONFIG_NODE_MGR_MESSENGER_PORT);
}
