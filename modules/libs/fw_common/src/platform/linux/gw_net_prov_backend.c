/* SPDX-License-Identifier: Apache-2.0 */

/*
 * TCP server backend for fw_common/gw_ble_prov, used by the Linux
 * gateway daemon for the network-based provisioning path.  Speaks the
 * same length-prefixed frame protocol defined in gw_net_prov.h and
 * dispatches into the platform-neutral protocol module exactly like the
 * BLE backend does.
 *
 * One client at a time; second concurrent connection is closed.  Frames
 * are processed sequentially on the per-connection thread, so the
 * protocol's single-slot decrypt arrangement holds naturally.
 *
 * mDNS announcement is delegated to a child `avahi-publish-service`
 * process (no libavahi-client dep needed at build time; avahi-daemon
 * is the canonical Debian/RPi mDNS responder).  Child is started in
 * gw_net_prov_start() and reaped in gw_net_prov_stop() / on commit
 * reboot.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fw_common/gw_ble_prov.h>
#include <fw_common/gw_net_prov.h>
#include <fw_common/log.h>

LOG_MODULE_REGISTER(gw_net_prov_be, LOG_LEVEL_INF);

/* ── module state ─────────────────────────────────────────────────── */

static int               g_listen_fd = -1;
static int               g_client_fd = -1;
static pid_t             g_avahi_pid;
static pthread_t         g_accept_thread;
static volatile sig_atomic_t g_running;
static pthread_mutex_t   g_proto_mu = PTHREAD_MUTEX_INITIALIZER;

/* ── frame I/O ────────────────────────────────────────────────────── */

static int read_full(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;
	while (n) {
		ssize_t got = recv(fd, p, n, 0);
		if (got == 0)  return -ECONNRESET;
		if (got < 0) {
			if (errno == EINTR) continue;
			return -errno;
		}
		p += got; n -= got;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	while (n) {
		ssize_t put = send(fd, p, n, MSG_NOSIGNAL);
		if (put <= 0) {
			if (put < 0 && errno == EINTR) continue;
			return put == 0 ? -ECONNRESET : -errno;
		}
		p += put; n -= put;
	}
	return 0;
}

static int send_frame(int fd, uint8_t type, const void *payload, size_t plen)
{
	if (plen > GW_NET_PROV_MAX_PAYLOAD) return -EMSGSIZE;
	uint8_t hdr[3];
	hdr[0] = type;
	hdr[1] = (uint8_t)((plen >> 8) & 0xff);
	hdr[2] = (uint8_t)(plen & 0xff);
	int rc = write_full(fd, hdr, sizeof hdr);
	if (rc < 0) return rc;
	if (plen) return write_full(fd, payload, plen);
	return 0;
}

static int recv_frame(int fd, uint8_t *type_out,
		      uint8_t *payload_buf, size_t *plen_out)
{
	uint8_t hdr[3];
	int rc = read_full(fd, hdr, sizeof hdr);
	if (rc < 0) return rc;
	*type_out = hdr[0];
	size_t plen = ((size_t)hdr[1] << 8) | hdr[2];
	if (plen > GW_NET_PROV_MAX_PAYLOAD) {
		LOG_WRN("frame plen %zu > max %d — dropping connection",
			plen, GW_NET_PROV_MAX_PAYLOAD);
		return -EMSGSIZE;
	}
	if (plen) {
		rc = read_full(fd, payload_buf, plen);
		if (rc < 0) return rc;
	}
	*plen_out = plen;
	return 0;
}

static void send_ack(int fd)        { send_frame(fd, GW_NET_REP_OK, NULL, 0); }
static void send_err(int fd, int rv)
{
	uint8_t e = (uint8_t)(rv < 0 ? -rv : rv);
	send_frame(fd, GW_NET_REP_ERR, &e, 1);
}

/* ── backend ops ──────────────────────────────────────────────────── */

static void be_notify_status(uint8_t status, void *user)
{
	(void)user;
	int fd = g_client_fd;
	if (fd >= 0) {
		(void)send_frame(fd, GW_NET_NOTIFY_STATUS, &status, 1);
	}
}

/* TCP path: decrypt + apply run inline on the connection thread, no
 * cross-thread scheduling needed.  We invoke gw_ble_prov_do_decrypt /
 * gw_ble_prov_do_apply directly from the relevant write handler. */
static void be_schedule_decrypt(void *user) { (void)user; }
static void be_schedule_apply  (void *user) { (void)user; }

static void *reboot_timer_thread(void *ud)
{
	(void)ud;
	sleep(1);
	LOG_INF("provisioning success — exiting (supervisor restarts us)");
	_exit(0);
}

static void be_schedule_reboot_1s(void *user)
{
	(void)user;
	pthread_t th;
	if (pthread_create(&th, NULL, reboot_timer_thread, NULL) != 0) {
		_exit(0);
	}
	pthread_detach(th);
}

static const struct gw_ble_prov_backend g_be = {
	.notify_status      = be_notify_status,
	.schedule_decrypt   = be_schedule_decrypt,
	.schedule_apply     = be_schedule_apply,
	.schedule_reboot_1s = be_schedule_reboot_1s,
	.user               = NULL,
};

/* ── per-connection handling ─────────────────────────────────────── */

static int dispatch_write_frame(uint8_t type,
				const uint8_t *payload, size_t plen)
{
	int rc;
	pthread_mutex_lock(&g_proto_mu);
	switch (type) {
	case GW_NET_REQ_HUB_X25519:
		rc = gw_ble_prov_write_hub_x25519(payload, plen);
		break;
	case GW_NET_REQ_WIFI_CRED:
		rc = gw_ble_prov_write_wifi_cred(payload, plen);
		if (rc == 0) gw_ble_prov_do_decrypt();
		break;
	case GW_NET_REQ_HUB_HOST:
		rc = gw_ble_prov_write_hub_host(payload, plen);
		if (rc == 0) gw_ble_prov_do_decrypt();
		break;
	case GW_NET_REQ_HUB_IDENTITY:
		rc = gw_ble_prov_write_hub_id(payload, plen);
		if (rc == 0) gw_ble_prov_do_decrypt();
		break;
	case GW_NET_REQ_OT_DATASET:
		rc = gw_ble_prov_write_ot_dataset(payload, plen);
		if (rc == 0) gw_ble_prov_do_decrypt();
		break;
	case GW_NET_REQ_COMMIT:
		rc = gw_ble_prov_write_commit(payload, plen);
		if (rc == 0 && plen == 1 && payload[0] == 0x01) {
			gw_ble_prov_do_apply();
		}
		break;
	default:
		rc = -EINVAL;
	}
	pthread_mutex_unlock(&g_proto_mu);
	return rc;
}

static void handle_one_client(int fd)
{
	g_client_fd = fd;
	LOG_INF("client connected — fd=%d", fd);

	uint8_t payload[GW_NET_PROV_MAX_PAYLOAD];

	for (;;) {
		uint8_t type;
		size_t  plen;
		int rc = recv_frame(fd, &type, payload, &plen);
		if (rc == -ECONNRESET) {
			LOG_INF("client closed connection");
			break;
		}
		if (rc < 0) {
			LOG_WRN("recv_frame: %d — closing", rc);
			break;
		}

		if (type == GW_NET_REQ_GET_INFO) {
			uint8_t info[GW_BLE_PROV_INFO_LEN];
			pthread_mutex_lock(&g_proto_mu);
			size_t n = gw_ble_prov_read_gw_info(info, sizeof info);
			pthread_mutex_unlock(&g_proto_mu);
			send_frame(fd, GW_NET_REP_INFO, info, n);
			continue;
		}

		int wrv = dispatch_write_frame(type, payload, plen);
		if (wrv < 0) {
			send_err(fd, wrv);
		} else {
			send_ack(fd);
		}
	}

	g_client_fd = -1;
	close(fd);
}

/* ── accept loop ──────────────────────────────────────────────────── */

static void *accept_loop(void *arg)
{
	(void)arg;
	while (g_running) {
		struct sockaddr_in cli;
		socklen_t len = sizeof cli;
		int fd = accept(g_listen_fd, (struct sockaddr *)&cli, &len);
		if (fd < 0) {
			if (errno == EINTR) continue;
			if (!g_running) break;
			LOG_WRN("accept: %s", strerror(errno));
			continue;
		}
		char addrbuf[64];
		inet_ntop(AF_INET, &cli.sin_addr, addrbuf, sizeof addrbuf);
		LOG_INF("connection from %s:%u", addrbuf, ntohs(cli.sin_port));

		/* If a client is already in flight, reject the second one.
		 * Provisioning is one-shot. */
		if (g_client_fd >= 0) {
			LOG_WRN("rejecting concurrent client (already serving)");
			close(fd);
			continue;
		}
		handle_one_client(fd);
	}
	return NULL;
}

/* ── avahi mDNS publish helper ────────────────────────────────────── */

static void spawn_avahi(const char *instance_name, uint16_t port)
{
	pid_t pid = fork();
	if (pid < 0) {
		LOG_WRN("fork(avahi-publish-service): %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		/* If the daemon dies (incl. SIGKILL), the kernel sends us
		 * SIGTERM so we don't leak an orphaned avahi-publish that
		 * keeps the old mDNS name on the LAN. */
		prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
		/* Re-check parent in case we missed a SIGKILL between fork
		 * and prctl. */
		if (getppid() == 1) _exit(0);
		char port_str[8];
		snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
		execlp("avahi-publish-service",
		       "avahi-publish-service",
		       "-s", instance_name, "_nn-gw._tcp", port_str,
		       (char *)NULL);
		/* Fall through on exec failure — avahi-publish-service may
		 * not be installed.  Daemon continues without mDNS. */
		_exit(127);
	}
	g_avahi_pid = pid;
	LOG_INF("avahi-publish-service '%s' on _nn-gw._tcp port %u (pid %d)",
		instance_name, port, (int)pid);
}

static void reap_avahi(void)
{
	if (g_avahi_pid > 0) {
		kill(g_avahi_pid, SIGTERM);
		int status;
		waitpid(g_avahi_pid, &status, 0);
		g_avahi_pid = 0;
	}
}

/* ── lifecycle ────────────────────────────────────────────────────── */

static void sig_handler(int signo)
{
	(void)signo;
	gw_net_prov_stop();
}

int gw_net_prov_start(uint16_t port, int mdns)
{
	if (port == 0) port = GW_NET_PROV_DEFAULT_PORT;

	/* Initialise the protocol module with this backend.  We use the
	 * Linux machine's hostname-derived bytes for the adv name; since
	 * the BLE backend doesn't run, MAC isn't strictly needed, but the
	 * protocol still expects 6 bytes for the gw_ble_prov_get_name()
	 * computation.  Take the last 6 bytes of the gateway-identity
	 * gw_id; gw_identity_init must have run before us. */
	uint8_t mac[6] = {0};
	{
		extern const uint8_t *gw_identity_get_id(void);
		const uint8_t *id = gw_identity_get_id();
		if (id) memcpy(mac, id, 6);
	}
	int rc = gw_ble_prov_init(&g_be, mac);
	if (rc) {
		LOG_ERR("gw_ble_prov_init: %d", rc);
		return rc;
	}
	const char *instance = gw_ble_prov_get_name();
	LOG_INF("net provisioning instance name: %s", instance);

	g_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (g_listen_fd < 0) {
		LOG_ERR("socket: %s", strerror(errno));
		return -errno;
	}
	int one = 1;
	setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

	struct sockaddr_in addr = {0};
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port        = htons(port);
	if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		LOG_ERR("bind(:%u): %s", port, strerror(errno));
		close(g_listen_fd); g_listen_fd = -1;
		return -errno;
	}
	if (listen(g_listen_fd, 1) < 0) {
		LOG_ERR("listen: %s", strerror(errno));
		close(g_listen_fd); g_listen_fd = -1;
		return -errno;
	}
	LOG_INF("TCP listener up on 0.0.0.0:%u", port);

	if (mdns) {
		spawn_avahi(instance, port);
	}

	g_running = 1;
	if (pthread_create(&g_accept_thread, NULL, accept_loop, NULL) != 0) {
		LOG_ERR("pthread_create(accept): %s", strerror(errno));
		close(g_listen_fd); g_listen_fd = -1;
		reap_avahi();
		return -errno;
	}

	signal(SIGINT,  sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGPIPE, SIG_IGN);
	return 0;
}

int gw_net_prov_run(void)
{
	if (!g_accept_thread) return -EINVAL;
	pthread_join(g_accept_thread, NULL);
	g_accept_thread = 0;
	return 0;
}

void gw_net_prov_stop(void)
{
	g_running = 0;
	if (g_listen_fd >= 0) {
		shutdown(g_listen_fd, SHUT_RDWR);
		close(g_listen_fd);
		g_listen_fd = -1;
	}
	if (g_client_fd >= 0) {
		shutdown(g_client_fd, SHUT_RDWR);
		/* don't close here — connection thread closes its own fd */
	}
	reap_avahi();
}
