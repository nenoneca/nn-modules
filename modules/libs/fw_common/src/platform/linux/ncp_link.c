/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Linux port of ncp_link — POSIX termios + pthread.
 *
 * Same lightweight Spinel dialect the C6 NCP firmware (apps/ncp_esp32c6)
 * implements; same TID arrangement (1..15 for requests, 0 for
 * unsolicited); same HDLC framing.  Sits on top of fw_common's hdlc.c +
 * spinel.c which are already platform-neutral.
 *
 * Cross-thread sync uses pthread_mutex / pthread_cond.  RX thread is a
 * regular pthread that does poll() on the UART fd; no IRQ involved.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <fw_common/hdlc.h>
#include <fw_common/log.h>
#include <fw_common/ncp_link.h>
#include <fw_common/spinel.h>

LOG_MODULE_REGISTER(ncp_link, LOG_LEVEL_INF);

#define TX_BUF_SZ      3072
#define MAX_TIDS       15

/* ── tid slot ─────────────────────────────────────────────────────── */

struct tid_slot {
	pthread_mutex_t mu;
	pthread_cond_t  done;
	bool            ready;
	uint8_t        *out_buf;
	size_t          out_cap;
	size_t          out_len;
	int             status;
	uint32_t        last_status_code;
	bool            in_use;
};

/* ── module state ─────────────────────────────────────────────────── */

static int                 g_fd = -1;
static struct hdlc_decoder g_hdlc;

static struct tid_slot     g_tids[MAX_TIDS + 1];
static pthread_mutex_t     g_tid_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t             g_next_tid = 1;

static struct ncp_link_stats g_stats;
static pthread_mutex_t     g_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t     g_tx_lock    = PTHREAD_MUTEX_INITIALIZER;

static ncp_link_udp_fwd_cb_t     g_udp_fwd_cb;
static ncp_link_stream_net_cb_t  g_stream_net_cb;

static pthread_t           g_rx_thread;
static atomic_int          g_stop;

static pthread_mutex_t     g_reset_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t      g_reset_cv = PTHREAD_COND_INITIALIZER;
static bool                g_reset_pending;

static atomic_int          g_quiet_for_ota;

/* ── small helpers ────────────────────────────────────────────────── */

static void monotonic_ms_from_now(uint32_t ms, struct timespec *out)
{
	clock_gettime(CLOCK_REALTIME, out);
	out->tv_sec  += ms / 1000;
	out->tv_nsec += (ms % 1000) * 1000000L;
	if (out->tv_nsec >= 1000000000L) {
		out->tv_sec  += 1;
		out->tv_nsec -= 1000000000L;
	}
}

static int wait_done(struct tid_slot *s, uint32_t timeout_ms)
{
	struct timespec deadline;
	monotonic_ms_from_now(timeout_ms, &deadline);
	pthread_mutex_lock(&s->mu);
	int rv = 0;
	while (!s->ready) {
		rv = pthread_cond_timedwait(&s->done, &s->mu, &deadline);
		if (rv == ETIMEDOUT) break;
	}
	pthread_mutex_unlock(&s->mu);
	return rv == 0 ? 0 : -ETIMEDOUT;
}

static void signal_done(struct tid_slot *s)
{
	pthread_mutex_lock(&s->mu);
	s->ready = true;
	pthread_cond_signal(&s->done);
	pthread_mutex_unlock(&s->mu);
}

static int reserve_tid(struct tid_slot **out_slot, uint8_t *out_tid,
		       uint8_t *out_buf, size_t out_cap)
{
	pthread_mutex_lock(&g_tid_lock);
	for (int i = 0; i < MAX_TIDS; i++) {
		uint8_t t = (uint8_t)(((g_next_tid - 1 + i) % MAX_TIDS) + 1);
		struct tid_slot *s = &g_tids[t];
		if (!s->in_use) {
			s->in_use = true;
			s->out_buf = out_buf;
			s->out_cap = out_cap;
			s->out_len = 0;
			s->status = -ETIMEDOUT;
			s->last_status_code = 0;
			s->ready = false;
			g_next_tid = (uint8_t)((t % MAX_TIDS) + 1);
			*out_slot = s;
			*out_tid = t;
			pthread_mutex_unlock(&g_tid_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_tid_lock);
	return -ENOMEM;
}

static void release_tid(struct tid_slot *s)
{
	pthread_mutex_lock(&g_tid_lock);
	s->in_use  = false;
	s->out_buf = NULL;
	s->out_cap = 0;
	pthread_mutex_unlock(&g_tid_lock);
}

/* ── dispatch ─────────────────────────────────────────────────────── */

static void dispatch_response(uint8_t tid, const struct spinel_frame *f)
{
	if (tid < 1 || tid > MAX_TIDS) return;
	struct tid_slot *s = &g_tids[tid];

	pthread_mutex_lock(&g_tid_lock);
	if (!s->in_use) {
		pthread_mutex_unlock(&g_tid_lock);
		LOG_DBG("reply for unowned tid=%u", tid);
		return;
	}

	if (!f->has_prop) {
		s->status = -EIO;
	} else if (f->prop == SPINEL_PROP_LAST_STATUS) {
		uint32_t code = 0;
		(void)spinel_unpack_uint(f->value, f->value_len, &code);
		s->last_status_code = code;
		if (code == SPINEL_STATUS_OK) {
			s->out_len = 0;
			s->status = 0;
		} else {
			LOG_WRN("NCP last-status for tid=%u: %u (%s)",
				tid, code, spinel_status_str(code));
			s->status = -EIO;
		}
	} else {
		size_t n = f->value_len < s->out_cap ? f->value_len : s->out_cap;
		if (s->out_buf && n > 0) memcpy(s->out_buf, f->value, n);
		s->out_len = n;
		s->status = 0;
	}
	pthread_mutex_unlock(&g_tid_lock);
	signal_done(s);
}

static ncp_link_prop_watch_cb_t g_prop_watch_cb;

static void handle_unsolicited(const struct spinel_frame *f)
{
	pthread_mutex_lock(&g_stats_lock);
	g_stats.rx_unsolicited++;
	pthread_mutex_unlock(&g_stats_lock);

	/* Scan results and other list-valued notifications arrive as
	 * PROP_VALUE_INSERTED, not IS: hand them to the watcher too. */
	if (f->cmd == SPINEL_CMD_PROP_VALUE_INSERTED && f->has_prop) {
		if (g_prop_watch_cb) {
			g_prop_watch_cb(f->prop, f->value, f->value_len);
		}
		return;
	}
	if (f->cmd == SPINEL_CMD_PROP_VALUE_IS && f->has_prop) {
		switch (f->prop) {
		case SPINEL_PROP_LAST_STATUS: {
			uint32_t code = 0;
			(void)spinel_unpack_uint(f->value, f->value_len, &code);
			LOG_INF("NCP unsolicited: last-status=%u (%s)",
				code, spinel_status_str(code));
			if (code >= 112 && code <= 127) {
				pthread_mutex_lock(&g_reset_mu);
				g_reset_pending = true;
				atomic_store(&g_quiet_for_ota, 0);
				pthread_cond_broadcast(&g_reset_cv);
				pthread_mutex_unlock(&g_reset_mu);
			}
			break;
		}
		case SPINEL_PROP_STREAM_NET:
			if (g_stream_net_cb) {
				g_stream_net_cb(f->value, f->value_len);
			}
			break;
		case SPINEL_PROP_THREAD_UDP_FORWARD_STREAM:
			if (g_udp_fwd_cb && f->value_len >= 2) {
				size_t plen = (size_t)f->value[0] |
					      ((size_t)f->value[1] << 8);
				if (f->value_len < 2 + plen + 2 + 16 + 2) break;
				const uint8_t *p = f->value + 2;
				uint16_t rport = (uint16_t)p[plen] |
						 ((uint16_t)p[plen + 1] << 8);
				const uint8_t *rip6 = p + plen + 2;
				uint16_t lport = (uint16_t)rip6[16] |
						 ((uint16_t)rip6[17] << 8);
				g_udp_fwd_cb(p, plen, rport, rip6, lport);
			}
			break;
		default:
			LOG_DBG("unsolicited IS prop=%u len=%zu",
				f->prop, f->value_len);
			if (g_prop_watch_cb) {
				g_prop_watch_cb(f->prop, f->value, f->value_len);
			}
			break;
		}
	}
}

static void on_hdlc_frame(const uint8_t *payload, size_t len, void *user)
{
	(void)user;
	pthread_mutex_lock(&g_stats_lock);
	g_stats.rx_frames++;
	g_stats.rx_bytes += (uint32_t)len;
	pthread_mutex_unlock(&g_stats_lock);

	struct spinel_frame f = {0};
	if (spinel_parse(payload, len, &f) < 0) {
		LOG_WRN("bad spinel frame (len=%zu)", len);
		return;
	}
	if (f.tid == 0) handle_unsolicited(&f);
	else            dispatch_response(f.tid, &f);
}

/* ── RX thread ────────────────────────────────────────────────────── */

static void *rx_thread_fn(void *arg)
{
	(void)arg;
	uint8_t buf[256];
	while (!atomic_load(&g_stop)) {
		struct pollfd pfd = { .fd = g_fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 250);
		if (pr <= 0) continue;
		if (pfd.revents & (POLLERR | POLLHUP)) {
			LOG_WRN("UART poll revents=0x%x", pfd.revents);
			continue;
		}
		ssize_t n = read(g_fd, buf, sizeof buf);
		if (n <= 0) {
			if (n < 0 && errno != EINTR) {
				LOG_WRN("UART read: %s", strerror(errno));
			}
			continue;
		}
		for (ssize_t i = 0; i < n; i++) {
			hdlc_decode_byte(&g_hdlc, buf[i]);
		}
	}
	return NULL;
}

/* ── TX ───────────────────────────────────────────────────────────── */

static int write_full(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w <= 0) {
			if (w < 0 && errno == EINTR) continue;
			return -errno;
		}
		p += w; n -= w;
	}
	return 0;
}

static int send_spinel(const uint8_t *payload, size_t payload_len)
{
	static uint8_t wire[TX_BUF_SZ];

	pthread_mutex_lock(&g_tx_lock);
	int n = hdlc_encode(payload, payload_len, wire, sizeof wire);
	if (n < 0) {
		pthread_mutex_unlock(&g_tx_lock);
		LOG_ERR("hdlc_encode: frame too large (%zu)", payload_len);
		return -EMSGSIZE;
	}
	int rv = write_full(g_fd, wire, (size_t)n);
	pthread_mutex_unlock(&g_tx_lock);
	if (rv < 0) return rv;

	pthread_mutex_lock(&g_stats_lock);
	g_stats.tx_frames++;
	g_stats.tx_bytes += (uint32_t)n;
	pthread_mutex_unlock(&g_stats_lock);
	return 0;
}

/* ── public API ───────────────────────────────────────────────────── */

int ncp_link_get(uint32_t prop, uint8_t *out_buf, size_t *out_len,
		 uint32_t timeout_ms)
{
	if (!out_len) return -EINVAL;
	struct tid_slot *slot;
	uint8_t tid;
	int rv = reserve_tid(&slot, &tid, out_buf, *out_len);
	if (rv < 0) { *out_len = 0; return rv; }

	uint8_t frame[32];
	int n = spinel_build_get(frame, sizeof frame, tid, prop);
	if (n < 0) { release_tid(slot); *out_len = 0; return -EMSGSIZE; }

	rv = send_spinel(frame, (size_t)n);
	if (rv < 0) { release_tid(slot); *out_len = 0; return rv; }

	if (wait_done(slot, timeout_ms) < 0) {
		release_tid(slot); *out_len = 0; return -ETIMEDOUT;
	}
	int status = slot->status;
	*out_len = slot->out_len;
	release_tid(slot);
	return status;
}

static int link_cmd_raw(uint32_t cmd, uint32_t prop,
			const uint8_t *payload, size_t payload_len,
			uint32_t *out_last_status, uint32_t timeout_ms)
{
	struct tid_slot *slot;
	uint8_t tid;
	int rv = reserve_tid(&slot, &tid, NULL, 0);
	if (rv < 0) return rv;

	/* The Zephyr impl holds tx_lock across encode + write to protect
	 * the static frame buffer.  We mirror that here by taking it
	 * before encoding and releasing inside send_spinel (which takes
	 * it again internally — pthread_mutex is non-recursive by default,
	 * so encode the frame under our own pre-lock, then release before
	 * send_spinel takes it). */
	static uint8_t frame[1300];
	static pthread_mutex_t s_encode_mu = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&s_encode_mu);
	int n = spinel_build_cmd(frame, sizeof frame, tid, cmd, prop,
				 payload, payload_len);
	if (n < 0) {
		pthread_mutex_unlock(&s_encode_mu);
		release_tid(slot);
		return -EMSGSIZE;
	}
	rv = send_spinel(frame, (size_t)n);
	pthread_mutex_unlock(&s_encode_mu);
	if (rv < 0) { release_tid(slot); return rv; }

	if (wait_done(slot, timeout_ms) < 0) {
		release_tid(slot); return -ETIMEDOUT;
	}
	int status = slot->status;
	if (out_last_status) *out_last_status = slot->last_status_code;
	release_tid(slot);
	return status;
}

int ncp_link_set_raw(uint32_t prop,
		     const uint8_t *payload, size_t payload_len,
		     uint32_t *out_last_status, uint32_t timeout_ms)
{
	return link_cmd_raw(SPINEL_CMD_PROP_VALUE_SET, prop, payload,
			    payload_len, out_last_status, timeout_ms);
}

int ncp_link_insert_raw(uint32_t prop,
			const uint8_t *payload, size_t payload_len,
			uint32_t *out_last_status, uint32_t timeout_ms)
{
	return link_cmd_raw(SPINEL_CMD_PROP_VALUE_INSERT, prop, payload,
			    payload_len, out_last_status, timeout_ms);
}

int ncp_link_remove_raw(uint32_t prop,
			const uint8_t *payload, size_t payload_len,
			uint32_t *out_last_status, uint32_t timeout_ms)
{
	return link_cmd_raw(SPINEL_CMD_PROP_VALUE_REMOVE, prop, payload,
			    payload_len, out_last_status, timeout_ms);
}

int ncp_link_set_bool(uint32_t prop, bool v, uint32_t timeout_ms)
{
	uint8_t b = v ? 1 : 0;
	return ncp_link_set_raw(prop, &b, 1, NULL, timeout_ms);
}

int ncp_link_set_u8(uint32_t prop, uint8_t v, uint32_t timeout_ms)
{
	return ncp_link_set_raw(prop, &v, 1, NULL, timeout_ms);
}

int ncp_link_set_u16(uint32_t prop, uint16_t v, uint32_t timeout_ms)
{
	uint8_t buf[2] = { (uint8_t)(v & 0xff), (uint8_t)(v >> 8) };
	return ncp_link_set_raw(prop, buf, 2, NULL, timeout_ms);
}

void ncp_link_stats_get(struct ncp_link_stats *out)
{
	pthread_mutex_lock(&g_stats_lock);
	*out = g_stats;
	pthread_mutex_unlock(&g_stats_lock);
}

void ncp_link_set_udp_fwd_cb(ncp_link_udp_fwd_cb_t cb)    { g_udp_fwd_cb = cb; }
void ncp_link_set_stream_net_cb(ncp_link_stream_net_cb_t cb) { g_stream_net_cb = cb; }
void ncp_link_set_prop_watch_cb(ncp_link_prop_watch_cb_t cb) { g_prop_watch_cb = cb; }

int ncp_link_udp_forward_tx(const uint8_t *payload, size_t payload_len,
			    uint16_t remote_port,
			    const uint8_t remote_ip6[16],
			    uint16_t local_port)
{
	if (payload_len > 1024) return -EMSGSIZE;
	static uint8_t body[1100];
	size_t n = 0;
	body[n++] = (uint8_t)(payload_len & 0xff);
	body[n++] = (uint8_t)(payload_len >> 8);
	memcpy(body + n, payload, payload_len); n += payload_len;
	body[n++] = (uint8_t)(remote_port & 0xff);
	body[n++] = (uint8_t)(remote_port >> 8);
	memcpy(body + n, remote_ip6, 16); n += 16;
	body[n++] = (uint8_t)(local_port & 0xff);
	body[n++] = (uint8_t)(local_port >> 8);
	uint32_t ls = 0;
	return ncp_link_set_raw(SPINEL_PROP_THREAD_UDP_FORWARD_STREAM,
				body, n, &ls, 500);
}

void ncp_link_arm_reset_signal(void)
{
	pthread_mutex_lock(&g_reset_mu);
	g_reset_pending = false;
	pthread_mutex_unlock(&g_reset_mu);
}

int ncp_link_wait_reset(uint32_t timeout_ms)
{
	struct timespec deadline;
	monotonic_ms_from_now(timeout_ms, &deadline);
	pthread_mutex_lock(&g_reset_mu);
	int rv = 0;
	while (!g_reset_pending) {
		rv = pthread_cond_timedwait(&g_reset_cv, &g_reset_mu, &deadline);
		if (rv == ETIMEDOUT) { pthread_mutex_unlock(&g_reset_mu); return -ETIMEDOUT; }
	}
	pthread_mutex_unlock(&g_reset_mu);
	return 0;
}

void ncp_link_set_quiet_for_ota(bool quiet) { atomic_store(&g_quiet_for_ota, quiet ? 1 : 0); }
bool ncp_link_is_quiet_for_ota(void)        { return atomic_load(&g_quiet_for_ota) != 0; }

/* ── Linux-side init ──────────────────────────────────────────────── */

/* UART line settings are host-profile dependent:
 *   NCP_UART_FLOW = rtscts (default) | none
 *       RPi5 header has RTS/CTS → hw flow control (the NCP firmware
 *       default build matches).  BeagleY-AI header has no RTS/CTS →
 *       'none' (pair with an NCP built with NN_GW_HOST=beagley-ai).
 *   NCP_UART_BAUD = 460800 (default) | 230400 | 115200
 */
static speed_t uart_baud_from_env(void)
{
	const char *s = getenv("NCP_UART_BAUD");

	if (!s || !s[0] || !strcmp(s, "460800")) return B460800;
	if (!strcmp(s, "230400")) return B230400;
	if (!strcmp(s, "115200")) return B115200;
	LOG_WRN("NCP_UART_BAUD '%s' unsupported, using 460800", s);
	return B460800;
}

static bool uart_rtscts_from_env(void)
{
	const char *s = getenv("NCP_UART_FLOW");

	if (!s || !s[0] || !strcmp(s, "rtscts")) return true;
	if (!strcmp(s, "none")) return false;
	LOG_WRN("NCP_UART_FLOW '%s' unsupported, using rtscts", s);
	return true;
}

int ncp_link_open_uart_linux(const char *path)
{
	speed_t baud  = uart_baud_from_env();
	bool rtscts   = uart_rtscts_from_env();

	int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) { LOG_ERR("open(%s): %s", path, strerror(errno)); return -errno; }
	struct termios tio;
	if (tcgetattr(fd, &tio) != 0) { close(fd); return -errno; }
	cfmakeraw(&tio);
	cfsetispeed(&tio, baud);
	cfsetospeed(&tio, baud);
	tio.c_cflag |= (CLOCAL | CREAD);
	if (rtscts)
		tio.c_cflag |= CRTSCTS;
	else
		tio.c_cflag &= ~CRTSCTS;
	tio.c_cflag &= ~(PARENB | CSTOPB);
	tio.c_cflag &= ~CSIZE;
	tio.c_cflag |= CS8;
	tio.c_cc[VMIN]  = 0;
	tio.c_cc[VTIME] = 0;
	if (tcsetattr(fd, TCSANOW, &tio) != 0) { close(fd); return -errno; }
	tcflush(fd, TCIOFLUSH);
	return fd;
}

int ncp_link_init_linux(const char *uart_dev)
{
	if (!uart_dev || !uart_dev[0]) return -EINVAL;
	if (g_fd >= 0) return 0;

	for (int i = 0; i <= MAX_TIDS; i++) {
		pthread_mutex_init(&g_tids[i].mu,   NULL);
		pthread_cond_init (&g_tids[i].done, NULL);
	}
	hdlc_decoder_init(&g_hdlc, on_hdlc_frame, NULL);

	int fd = ncp_link_open_uart_linux(uart_dev);
	if (fd < 0) return fd;
	g_fd = fd;

	atomic_store(&g_stop, 0);
	if (pthread_create(&g_rx_thread, NULL, rx_thread_fn, NULL) != 0) {
		LOG_ERR("pthread_create(rx): %s", strerror(errno));
		close(g_fd); g_fd = -1;
		return -errno;
	}
	pthread_setname_np(g_rx_thread, "ncp_rx");

	LOG_INF("ncp link up on %s", uart_dev);
	return 0;
}
