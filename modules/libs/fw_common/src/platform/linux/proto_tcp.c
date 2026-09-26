/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Linux port of proto_tcp — POSIX socket + pthread worker maintaining a
 * single TCP connection to the hub for the gateway-side nn_proto
 * transport.  API-compatible with the Zephyr implementation in
 * apps/ncp_host_esp32c6/src/proto_tcp.c so app code only sees one header.
 *
 * Differences from the Zephyr impl:
 *   - k_thread → pthread
 *   - k_sem    → pthread_mutex + pthread_cond
 *   - ring_buf → a fixed-capacity byte queue protected by tx_mu
 *   - zsock_*  → plain POSIX socket/connect/send/recv/poll
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <nn_proto/nn_proto.h>

#include <fw_common/log.h>
#include <fw_common/proto_tcp.h>

LOG_MODULE_REGISTER(proto_tcp, LOG_LEVEL_INF);

/* ── tunables ─────────────────────────────────────────────────────── */

#define TX_RING_BYTES        16384u
#define RX_BUF_BYTES         (NN_PROTO_HEADER_FIXED + 1280 + NN_PROTO_SIG_LEN + 256)
#define RECONNECT_INITIAL_MS 500u
#define RECONNECT_MAX_MS     30000u
#define HOSTNAME_MAX         64u

/* ── ring buffer (byte-oriented, single producer single consumer) ── */

struct tx_ring {
	uint8_t  buf[TX_RING_BYTES];
	uint32_t head;
	uint32_t tail;
	uint32_t used;
};

static uint32_t tr_space(const struct tx_ring *r)
{
	return TX_RING_BYTES - r->used;
}

static void tr_put(struct tx_ring *r, const void *src, uint32_t n)
{
	const uint8_t *s = src;
	while (n) {
		uint32_t chunk = TX_RING_BYTES - r->head;
		if (chunk > n) chunk = n;
		memcpy(&r->buf[r->head], s, chunk);
		r->head = (r->head + chunk) % TX_RING_BYTES;
		s += chunk; n -= chunk;
		r->used += chunk;
	}
}

static uint32_t tr_peek(const struct tx_ring *r, void *dst, uint32_t n)
{
	uint32_t want = n > r->used ? r->used : n;
	uint32_t left = want;
	uint8_t *d = dst;
	uint32_t tail = r->tail;
	while (left) {
		uint32_t chunk = TX_RING_BYTES - tail;
		if (chunk > left) chunk = left;
		memcpy(d, &r->buf[tail], chunk);
		tail = (tail + chunk) % TX_RING_BYTES;
		d += chunk; left -= chunk;
	}
	return want;
}

static uint32_t tr_get(struct tx_ring *r, void *dst, uint32_t n)
{
	uint32_t got = tr_peek(r, dst, n);
	r->tail = (r->tail + got) % TX_RING_BYTES;
	r->used -= got;
	return got;
}

static void tr_reset(struct tx_ring *r)
{
	r->head = r->tail = r->used = 0;
}

/* ── module state ─────────────────────────────────────────────────── */

static struct {
	struct proto_tcp_config cfg;
	char                    hostname[HOSTNAME_MAX];
	int                     fd;
	enum proto_tcp_state    state;
	uint32_t                next_backoff_ms;
	struct proto_tcp_stats  stats;

	struct tx_ring          tx;
	pthread_mutex_t         tx_mu;
	pthread_cond_t          tx_cv;

	uint8_t                 rx_buf[RX_BUF_BYTES];
	size_t                  rx_len;

	pthread_t               worker;
	atomic_int              stop;
} S;

/* ── socket helpers ──────────────────────────────────────────────── */

static void close_socket(void)
{
	if (S.fd >= 0) {
		close(S.fd);
		S.fd = -1;
		S.stats.disconnects++;
	}
	S.state = PROTO_TCP_DOWN;
}

static int try_connect(void)
{
	struct addrinfo hints = {0};
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char port_str[8];
	snprintf(port_str, sizeof port_str, "%u", S.cfg.hub_port);

	S.state = PROTO_TCP_CONNECTING;

	struct addrinfo *res = NULL;
	int rv = getaddrinfo(S.hostname, port_str, &hints, &res);
	if (rv != 0 || !res) {
		LOG_WRN("getaddrinfo(%s): %s", S.hostname, gai_strerror(rv));
		S.stats.connect_failures++;
		return -EHOSTUNREACH;
	}

	int fd = -1;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC,
			    ai->ai_protocol);
		if (fd < 0) continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			break;
		}
		LOG_WRN("connect(%s:%s): %s", S.hostname, port_str,
			strerror(errno));
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);

	if (fd < 0) {
		S.stats.connect_failures++;
		return -ECONNREFUSED;
	}

	int one = 1;
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

	S.fd = fd;
	S.state = PROTO_TCP_UP;
	S.next_backoff_ms = RECONNECT_INITIAL_MS;
	S.stats.connects++;
	LOG_INF("TCP connected to %s:%s (fd=%d)", S.hostname, port_str, fd);
	return 0;
}

static uint8_t s_frame_buf[NN_PROTO_HEADER_FIXED + 1280 + NN_PROTO_SIG_LEN];

static int drain_tx(void)
{
	for (;;) {
		uint8_t len_hdr[4];
		uint32_t flen = 0;
		pthread_mutex_lock(&S.tx_mu);
		uint32_t got = tr_peek(&S.tx, len_hdr, sizeof len_hdr);
		if (got < sizeof len_hdr) {
			pthread_mutex_unlock(&S.tx_mu);
			return 0;
		}
		flen = (uint32_t)len_hdr[0]
		     | ((uint32_t)len_hdr[1] << 8)
		     | ((uint32_t)len_hdr[2] << 16)
		     | ((uint32_t)len_hdr[3] << 24);
		if (flen > sizeof s_frame_buf - sizeof len_hdr ||
		    flen > TX_RING_BYTES - sizeof len_hdr) {
			LOG_ERR("frame too large to drain: %u", flen);
			tr_reset(&S.tx);
			S.stats.tx_drops_socket++;
			pthread_mutex_unlock(&S.tx_mu);
			return 0;
		}
		uint32_t total = sizeof len_hdr + flen;
		tr_get(&S.tx, s_frame_buf, total);
		pthread_mutex_unlock(&S.tx_mu);

		const uint8_t *p = s_frame_buf + sizeof len_hdr;
		size_t left = flen;
		while (left) {
			ssize_t n = send(S.fd, p, left, MSG_NOSIGNAL);
			if (n <= 0) {
				if (n < 0 && errno == EINTR) continue;
				LOG_WRN("send: rv=%zd errno=%d", n, errno);
				S.stats.tx_drops_socket++;
				return -EIO;
			}
			p    += n;
			left -= (size_t)n;
		}
		S.stats.tx_frames++;
		S.stats.tx_bytes += flen;
	}
}

static int parse_rx_buf(void)
{
	for (;;) {
		struct nn_proto_view view;
		size_t consumed = 0;
		int rv = nn_proto_parse(S.rx_buf, S.rx_len, &view, &consumed);
		if (rv == -ENOSPC) return 0;
		if (rv != 0) {
			LOG_WRN("nn_proto_parse: %d (rx_len=%zu)", rv, S.rx_len);
			S.stats.rx_drops_bad++;
			return -EIO;
		}
		S.stats.rx_frames++;
		S.stats.rx_bytes += consumed;
		if (S.cfg.on_rx) {
			S.cfg.on_rx(S.rx_buf, consumed, S.cfg.on_rx_user);
		}
		size_t remaining = S.rx_len - consumed;
		if (remaining) memmove(S.rx_buf, S.rx_buf + consumed, remaining);
		S.rx_len = remaining;
	}
}

static int recv_some(void)
{
	if (S.rx_len >= sizeof S.rx_buf) {
		LOG_ERR("rx_buf full (%zu B); dropping connection", S.rx_len);
		S.stats.rx_drops_oversize++;
		return -EIO;
	}

	struct pollfd pfd = { .fd = S.fd, .events = POLLIN };
	int rv = poll(&pfd, 1, 100);
	if (rv < 0) {
		if (errno == EINTR) return 0;
		return -errno;
	}
	if (rv == 0) return 0;
	if (pfd.revents & (POLLERR | POLLHUP)) {
		LOG_WRN("poll: revents=0x%x", pfd.revents);
		return -EIO;
	}

	ssize_t n = recv(S.fd, S.rx_buf + S.rx_len,
			 sizeof S.rx_buf - S.rx_len, 0);
	if (n < 0) {
		LOG_WRN("recv: errno=%d", errno);
		return -EIO;
	}
	if (n == 0) {
		LOG_INF("peer closed");
		return -EIO;
	}
	S.rx_len += (size_t)n;
	return parse_rx_buf();
}

/* ── worker thread ───────────────────────────────────────────────── */

static void *worker_main(void *arg)
{
	(void)arg;
	/* Small initial settle so caller can finish setup. */
	struct timespec settle = { .tv_sec = 0, .tv_nsec = 500 * 1000 * 1000 };
	nanosleep(&settle, NULL);

	while (!atomic_load(&S.stop)) {
		if (S.state != PROTO_TCP_UP) {
			int rv = try_connect();
			if (rv < 0) {
				uint32_t wait_ms = S.next_backoff_ms;
				LOG_INF("backoff %u ms", wait_ms);
				struct timespec ts = {
					.tv_sec  = wait_ms / 1000,
					.tv_nsec = (wait_ms % 1000) * 1000 * 1000,
				};
				nanosleep(&ts, NULL);
				S.next_backoff_ms = S.next_backoff_ms * 2u;
				if (S.next_backoff_ms > RECONNECT_MAX_MS) {
					S.next_backoff_ms = RECONNECT_MAX_MS;
				}
				continue;
			}
			S.rx_len = 0;
		}

		int rv = drain_tx();
		if (rv < 0) { close_socket(); continue; }
		rv = recv_some();
		if (rv < 0) { close_socket(); continue; }

		/* Wait briefly for a TX signal or RX poll cycle. */
		pthread_mutex_lock(&S.tx_mu);
		if (S.tx.used == 0) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 50 * 1000 * 1000;  /* 50 ms */
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec += 1;
				ts.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&S.tx_cv, &S.tx_mu, &ts);
		}
		pthread_mutex_unlock(&S.tx_mu);
	}
	close_socket();
	return NULL;
}

/* ── public API ───────────────────────────────────────────────────── */

int proto_tcp_init(const struct proto_tcp_config *cfg)
{
	if (!cfg || !cfg->hub_hostname || !cfg->hub_port) return -EINVAL;
	memset(&S, 0, sizeof S);
	S.cfg = *cfg;
	S.fd  = -1;
	S.next_backoff_ms = RECONNECT_INITIAL_MS;
	snprintf(S.hostname, sizeof S.hostname, "%s", cfg->hub_hostname);
	pthread_mutex_init(&S.tx_mu, NULL);
	pthread_cond_init (&S.tx_cv, NULL);

	if (pthread_create(&S.worker, NULL, worker_main, NULL) != 0) {
		LOG_ERR("pthread_create: %s", strerror(errno));
		return -errno;
	}
	pthread_setname_np(S.worker, "proto_tcp");
	LOG_INF("proto_tcp init: hub=%s:%u", S.hostname, S.cfg.hub_port);
	return 0;
}

int proto_tcp_set_hub_hostname(const char *hostname)
{
	const char *new_h = (hostname && hostname[0]) ?
			    hostname : S.cfg.hub_hostname;
	if (!new_h) return -EINVAL;
	if (strncmp(new_h, S.hostname, sizeof S.hostname) == 0) return 0;
	snprintf(S.hostname, sizeof S.hostname, "%s", new_h);
	LOG_INF("hub hostname → %s; forcing reconnect", S.hostname);
	close_socket();
	S.next_backoff_ms = RECONNECT_INITIAL_MS;
	pthread_mutex_lock(&S.tx_mu);
	pthread_cond_signal(&S.tx_cv);
	pthread_mutex_unlock(&S.tx_mu);
	return 0;
}

int proto_tcp_enqueue(const uint8_t *frame, size_t len)
{
	if (!frame || len == 0 || len > 0xFFFFFFFFu) return -EINVAL;
	uint8_t hdr[4] = {
		(uint8_t)(len & 0xff),
		(uint8_t)((len >> 8) & 0xff),
		(uint8_t)((len >> 16) & 0xff),
		(uint8_t)((len >> 24) & 0xff),
	};

	pthread_mutex_lock(&S.tx_mu);
	uint32_t free_bytes = tr_space(&S.tx);
	if (free_bytes < sizeof hdr + (uint32_t)len) {
		S.stats.tx_drops_full++;
		pthread_mutex_unlock(&S.tx_mu);
		return -ENOMEM;
	}
	tr_put(&S.tx, hdr,   sizeof hdr);
	tr_put(&S.tx, frame, (uint32_t)len);
	pthread_cond_signal(&S.tx_cv);
	pthread_mutex_unlock(&S.tx_mu);
	return 0;
}

enum proto_tcp_state proto_tcp_get_state(void)
{
	return S.state;
}

void proto_tcp_get_stats(struct proto_tcp_stats *out)
{
	if (out) *out = S.stats;
}
