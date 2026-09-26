/* SPDX-License-Identifier: Apache-2.0 */

/*
 * gw_linux — Linux gateway-host daemon, currently a smoke-test harness
 * for the shared fw_common modules.
 *
 * Subcommands:
 *   spinel [/dev/ttyAMA3]
 *       Open the given UART (default /dev/ttyAMA3) at 460800 8N1 +RTSCTS,
 *       send a Spinel NOOP frame via fw_common HDLC+Spinel, wait for the
 *       ACK frame, verify LAST_STATUS = OK.
 *
 *   crypto
 *       Initialise fw_common/hub_crypto on this Linux host, generate
 *       (or load) the device's X25519 keypair, set the "hub" pubkey
 *       to a copy of the device's own pubkey (self-loop), then
 *       encrypt + decrypt a short plaintext through the same envelope
 *       parser/builder used by the Zephyr firmware.  Verifies the PSA
 *       crypto path + the new envelope/base64/kvstore plumbing.
 *
 *   identity-import <64 hex chars | -> [--force]
 *       Adopt a P-256 private key minted elsewhere as THIS gateway's
 *       identity (kv gw_identity/p256_priv), so the gateway_id the hub
 *       already registered is the one this daemon presents.  A camera
 *       host gets its dormant gateway identity delivered with its own
 *       provisioning (the 0x47 gw_blob TLV) and seeds it here before the
 *       first start; without this step gw_identity_init() would mint a
 *       fresh key and the hub would see a second, unknown gateway with
 *       the same label -- which is exactly how the duplicate "cam3-gw"
 *       records came to exist.  Refuses to replace a different existing
 *       key unless --force is given.  Prints the resulting gateway_id.
 *
 *   default = spinel
 */

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <poll.h>
#include <pthread.h>

#include <fw_common/gw_ble_prov_linux.h>
#include <fw_common/gw_identity.h>
#include <fw_common/kvstore.h>
#include <fw_common/gw_net_prov.h>
#include <fw_common/gw_ot_apply.h>
#include <fw_common/gw_provision.h>
#include <fw_common/hdlc.h>
#include <fw_common/hub_crypto.h>
#include <fw_common/log.h>
#include <fw_common/ncp_link.h>
#include <fw_common/proto_router.h>
#include <fw_common/proto_tcp.h>
#include <fw_common/proto_udp.h>
#include <fw_common/spinel.h>

#include <nn_proto/nn_proto.h>

LOG_MODULE_REGISTER(gw_linux_main, LOG_LEVEL_INF);

/* ── Spinel NOOP smoke test ────────────────────────────────────────── */

/* UART open/config lives in fw_common (ncp_link_open_uart_linux) and is
 * host-profile aware via NCP_UART_FLOW / NCP_UART_BAUD. */

static volatile uint8_t g_rx[64];
static volatile size_t  g_rx_len;
static volatile int     g_frame_seen;

static void on_frame(const uint8_t *payload, size_t len, void *user)
{
	(void)user;
	if (len > sizeof(g_rx)) return;
	memcpy((uint8_t *)g_rx, payload, len);
	g_rx_len     = len;
	g_frame_seen = 1;
}

/* Send one Spinel NOOP on `port` and wait up to `timeout_ms` for the
 * ACK frame.  Returns 0 when the NCP answered.  Used both by the
 * `spinel` smoke test and by runtime port discovery. */
static int spinel_noop_probe(const char *port, int timeout_ms, bool verbose)
{
	int fd = ncp_link_open_uart_linux(port);
	if (fd < 0) return 1;
	if (verbose) LOG_INF("opened %s (NCP_UART_FLOW=%s NCP_UART_BAUD=%s)",
			     port,
			     getenv("NCP_UART_FLOW") ? getenv("NCP_UART_FLOW") : "rtscts",
			     getenv("NCP_UART_BAUD") ? getenv("NCP_UART_BAUD") : "460800");

	uint8_t spinel[2] = { SPINEL_HEADER_FLAG | 0x01, SPINEL_CMD_NOOP };
	uint8_t wire[32];
	int n = hdlc_encode(spinel, sizeof(spinel), wire, sizeof(wire));
	if (n <= 0) { close(fd); return 2; }

	if (verbose) LOG_INF("TX %d B Spinel NOOP", n);
	if (write(fd, wire, (size_t)n) != (ssize_t)n) { close(fd); return 3; }

	g_frame_seen = 0;
	struct hdlc_decoder dec;
	hdlc_decoder_init(&dec, on_frame, NULL);
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
	while (!g_frame_seen) {
		struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
		long ms = (now.tv_sec - t0.tv_sec) * 1000L +
			  (now.tv_nsec - t0.tv_nsec) / 1000000L;
		if (ms > timeout_ms) {
			if (verbose) LOG_ERR("no Spinel reply within %d ms", timeout_ms);
			close(fd); return 4;
		}
		int pr = poll(&pfd, 1, timeout_ms - (int)ms);
		if (pr <= 0) continue;
		uint8_t rbuf[64];
		ssize_t got = read(fd, rbuf, sizeof(rbuf));
		for (ssize_t i = 0; i < got; i++) hdlc_decode_byte(&dec, rbuf[i]);
	}
	if (g_rx_len < 4 ||
	    g_rx[0] != (SPINEL_HEADER_FLAG | 0x01) ||
	    g_rx[1] != SPINEL_CMD_PROP_VALUE_IS ||
	    g_rx[2] != SPINEL_PROP_LAST_STATUS) {
		if (verbose) LOG_ERR("unexpected Spinel reply");
		close(fd); return 5;
	}
	if (verbose) LOG_INF("NOOP round-trip OK — LAST_STATUS = 0x%02x", g_rx[3]);
	close(fd);
	return 0;
}

static int do_spinel(const char *port)
{
	return spinel_noop_probe(port, 1500, true);
}

/* ── orderly stop ─────────────────────────────────────────────────────
 * systemd's stop (or an operator's Ctrl-C) must not strand the mesh:
 * the NCP would keep running Thread as a router with its children after
 * this process is gone.  The flag ends the operate loop, which then
 * takes the stack down before exiting. */
static volatile sig_atomic_t g_stop;

static void on_stop_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void arm_stop_signals(void)
{
	struct sigaction sa = { 0 };
	sa.sa_handler = on_stop_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
}

/* ── runtime NCP port discovery ────────────────────────────────────
 *
 * ttyACMn/ttyAMAn numbering drifts across gateway hosts and reboots,
 * so the port is resolved at runtime:
 *   1. explicit CLI arg (unless "auto")
 *   2. $NCP_UART        (unless "auto")
 *   3. probe candidates with a Spinel NOOP; first responder wins:
 *        /dev/serial/by-id/ *Espressif*   (USB-JTAG, stable by MAC)
 *        /dev/ttyAMA0..3                  (header UARTs)
 *        /dev/ttyACM0..3                  (fallback when by-id absent)
 */
static const char *resolve_ncp_port(const char *cli_arg, char *buf, size_t buf_len)
{
	if (cli_arg && strcmp(cli_arg, "auto") != 0)
		return cli_arg;
	const char *env = getenv("NCP_UART");
	if (env && env[0] && strcmp(env, "auto") != 0)
		return env;

	glob_t gl;
	if (glob("/dev/serial/by-id/*Espressif*", 0, NULL, &gl) == 0) {
		for (size_t i = 0; i < gl.gl_pathc; i++) {
			LOG_INF("probing %s …", gl.gl_pathv[i]);
			if (spinel_noop_probe(gl.gl_pathv[i], 700, false) == 0) {
				snprintf(buf, buf_len, "%s", gl.gl_pathv[i]);
				globfree(&gl);
				LOG_INF("NCP discovered on %s", buf);
				return buf;
			}
		}
	}
	globfree(&gl);

	static const char *const fixed[] = {
		"/dev/ttyAMA0", "/dev/ttyAMA1", "/dev/ttyAMA2", "/dev/ttyAMA3",
		"/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2", "/dev/ttyACM3",
	};
	for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
		if (access(fixed[i], R_OK | W_OK) != 0)
			continue;
		LOG_INF("probing %s …", fixed[i]);
		if (spinel_noop_probe(fixed[i], 700, false) == 0) {
			snprintf(buf, buf_len, "%s", fixed[i]);
			LOG_INF("NCP discovered on %s", buf);
			return buf;
		}
	}

	LOG_ERR("no NCP found (set NCP_UART or pass the port explicitly)");
	return NULL;
}

/* ── hub_crypto self-loop smoke test ───────────────────────────────── */

static int do_crypto(void)
{
	LOG_INF("hub_crypto self-loop on Linux");
	int rc = hub_crypto_init();
	if (rc) { LOG_ERR("hub_crypto_init: %d", rc); return 11; }

	/* Use the device's own X25519 pubkey as the "hub" pubkey so we
	 * can drive a self-encrypt → self-decrypt round-trip through the
	 * shared ECIES path. */
	uint8_t dev_pub[32];
	hub_crypto_get_device_x25519_pub(dev_pub);
	rc = hub_crypto_set_hub_pubkey(dev_pub);
	if (rc) { LOG_ERR("hub_crypto_set_hub_pubkey: %d", rc); return 12; }

	const char plaintext[] = "{\"hello\":\"from gw_linux\"}";
	char envelope[1024];
	rc = hub_crypto_encrypt((const uint8_t *)plaintext, sizeof(plaintext) - 1,
				envelope, sizeof(envelope));
	if (rc) { LOG_ERR("hub_crypto_encrypt: %d", rc); return 13; }
	LOG_INF("encrypted envelope (%zu chars):", strlen(envelope));
	fprintf(stderr, "  %s\n", envelope);

	/* hub_crypto_decrypt parses the H2D direction (info=h2d).  Since
	 * we encrypted as D2H (the only direction we have on the device
	 * side), running decrypt on it should NOT verify (auth-fail) —
	 * but the envelope parse + base64 decode path runs fully, which
	 * is the part we want to smoke-test.  Better self-loop: encrypt
	 * with D2H, decrypt with D2H — which means we'd need a separate
	 * "device-side decrypt" path, currently not in hub_crypto.
	 *
	 * For this smoke test we just verify encrypt succeeds + produces
	 * a parseable envelope.  A future round-trip test using the hub
	 * keypair (or a test-mode decrypt with D2H info tag) would
	 * close the loop. */
	uint8_t plain_out[256];
	size_t  plain_out_len = sizeof(plain_out);
	rc = hub_crypto_decrypt(envelope, plain_out, &plain_out_len);
	if (rc == -EACCES) {
		LOG_INF("decrypt expectedly returned -EACCES (D2H envelope "
			"can't be decrypted via the H2D path — auth tag "
			"derived from a different HKDF info)");
		LOG_INF("crypto self-loop: encrypt OK, decrypt-path exercised");
		return 0;
	}
	if (rc == 0) {
		LOG_INF("decrypt produced %zu bytes (unexpected for D2H/H2D "
			"asymmetry — check HKDF info tags)", plain_out_len);
		return 0;
	}
	LOG_ERR("hub_crypto_decrypt failed unexpectedly: %d", rc);
	return 14;
}

/* ── gw_identity sign/verify smoke test ────────────────────────────── */

static int do_identity(void)
{
	LOG_INF("gw_identity smoke test on Linux");
	int rc = gw_identity_init();
	if (rc) { LOG_ERR("gw_identity_init: %d", rc); return 21; }

	const uint8_t *id  = gw_identity_get_id();
	const uint8_t *pub = gw_identity_get_pubkey();
	if (!id || !pub) {
		LOG_ERR("getters returned NULL after init");
		return 22;
	}
	fprintf(stderr, "  gateway_id : ");
	for (int i = 0; i < GW_IDENTITY_ID_LEN; i++) fprintf(stderr, "%02x", id[i]);
	fprintf(stderr, "\n  p256_pub   : ");
	for (int i = 0; i < GW_IDENTITY_PUBKEY_LEN; i++)
		fprintf(stderr, "%02x", pub[i]);
	fprintf(stderr, "\n");

	const uint8_t msg[] = "nn_proto signtest (linux daemon)";
	uint8_t sig[GW_IDENTITY_SIG_LEN];

	rc = gw_identity_sign(NULL, msg, sizeof(msg) - 1, sig);
	if (rc) { LOG_ERR("sign: %d", rc); return 23; }
	LOG_INF("sign OK (sig length=%d)", GW_IDENTITY_SIG_LEN);

	rc = gw_identity_verify((void *)pub, msg, sizeof(msg) - 1, sig);
	if (rc) { LOG_ERR("verify: %d", rc); return 24; }
	LOG_INF("verify OK — sign+verify round-trip complete on Linux");

	/* Negative test: a tampered byte should fail verify. */
	uint8_t bad_msg[sizeof(msg) - 1];
	memcpy(bad_msg, msg, sizeof(bad_msg));
	bad_msg[0] ^= 0xff;
	rc = gw_identity_verify((void *)pub, bad_msg, sizeof(bad_msg), sig);
	if (rc == -EBADMSG) {
		LOG_INF("tamper detection OK (verify -> -EBADMSG)");
	} else {
		LOG_ERR("tamper test unexpected rv=%d", rc);
		return 25;
	}
	return 0;
}

/* ── identity-import: adopt a pre-minted P-256 key ─────────────────── */

static uint8_t s_existing_priv[GW_IDENTITY_PRIV_LEN];
static bool    s_existing_seen;

static int identity_import_load_cb(const char *suffix, const uint8_t *value,
				   size_t value_len, void *user)
{
	(void)user;
	if (strcmp(suffix, "p256_priv") == 0 && value_len == GW_IDENTITY_PRIV_LEN) {
		memcpy(s_existing_priv, value, GW_IDENTITY_PRIV_LEN);
		s_existing_seen = true;
	}
	return 0;
}

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int do_identity_import(const char *hex, bool force)
{
	uint8_t priv[GW_IDENTITY_PRIV_LEN];
	char line[2 * GW_IDENTITY_PRIV_LEN + 8];
	if (hex && !strcmp(hex, "-")) {
		/* From stdin, so the key never appears in any argv. */
		if (!fgets(line, sizeof line, stdin)) {
			LOG_ERR("identity-import: nothing on stdin");
			return 70;
		}
		line[strcspn(line, "\r\n")] = 0;
		hex = line;
	}
	if (!hex || strlen(hex) != 2 * GW_IDENTITY_PRIV_LEN) {
		LOG_ERR("identity-import: need exactly %d hex chars",
			2 * GW_IDENTITY_PRIV_LEN);
		return 70;
	}
	for (int i = 0; i < GW_IDENTITY_PRIV_LEN; i++) {
		int hi = hex_nibble(hex[2 * i]), lo = hex_nibble(hex[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			LOG_ERR("identity-import: not hex at offset %d", 2 * i);
			return 70;
		}
		priv[i] = (uint8_t)((hi << 4) | lo);
	}

	int rc = fw_kv_init();
	if (rc) { LOG_ERR("fw_kv_init: %d", rc); return 71; }
	rc = fw_kv_register("gw_identity", identity_import_load_cb, NULL);
	if (rc) { LOG_ERR("fw_kv_register: %d", rc); return 71; }
	(void)fw_kv_load_all();

	if (s_existing_seen && memcmp(s_existing_priv, priv, sizeof priv) == 0) {
		LOG_INF("identity already imported -- nothing to do");
	} else if (s_existing_seen && !force) {
		LOG_ERR("a DIFFERENT identity is already stored; the hub knows "
			"this gateway by it.  Re-run with --force to replace it.");
		return 72;
	} else {
		rc = fw_kv_save("gw_identity/p256_priv", priv, sizeof priv);
		if (rc) { LOG_ERR("fw_kv_save: %d", rc); return 73; }
		LOG_INF("identity imported%s", s_existing_seen ? " (replaced)" : "");
	}
	memset(priv, 0, sizeof priv);
	memset(line, 0, sizeof line);

	/* Derive and print the id the hub will see, so a caller can compare
	 * it with the one it was handed. */
	rc = gw_identity_init();
	if (rc) { LOG_ERR("gw_identity_init: %d", rc); return 74; }
	const uint8_t *id = gw_identity_get_id();
	for (int i = 0; i < GW_IDENTITY_ID_LEN; i++) printf("%02x", id[i]);
	printf("\n");
	return 0;
}


/* ── mesh table snapshots (diagnostics) ──────────────────────────────
 * Every 30 s: the NCP's children with the addresses each registered, and
 * its EID -> RLOC16 address cache.  Together with the per-frame H2D line
 * this says, for any ack a sensor never got, whether this router still
 * believed the sensor was its child, or which router it thought the
 * sensor sat behind. */
#define SPINEL_PROP_THREAD_CHILD_TABLE_ADDRESSES 0x1521
#define SPINEL_PROP_THREAD_ADDRESS_CACHE_TABLE   0x1523
#define SPINEL_PROP_THREAD_ROUTER_TABLE_X        0x1517
#define SPINEL_PROP_CNTR_ALL_MAC_COUNTERS_X      0x0691
#define SPINEL_PROP_CNTR_IP_TX_SUCCESS_X         0x0630
#define SPINEL_PROP_CNTR_IP_TX_FAILURE_X         0x0632

static const char *ip6_tail(const uint8_t a[16], char *buf, size_t n)
{
	/* the last 4 groups identify an EID/ML-EID well enough */
	snprintf(buf, n, "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
		 a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
	return buf;
}

static void log_mesh_tables(void)
{
	static uint8_t buf[2048];
	size_t len = sizeof buf;
	char line[1024], t[40];
	if (ncp_link_get(SPINEL_PROP_THREAD_CHILD_TABLE_ADDRESSES, buf, &len, 2000) == 0) {
		size_t i = 0; int n = 0; int off = 0;
		off += snprintf(line + off, sizeof line - off, "children:");
		while (i + 2 <= len) {
			uint16_t sl = (uint16_t)(buf[i] | (buf[i + 1] << 8));
			const uint8_t *e = buf + i + 2;
			if (i + 2 + sl > len || sl < 10) break;
			uint16_t rloc = (uint16_t)(e[8] | (e[9] << 8));
			off += snprintf(line + off, sizeof line - off, " [0x%04x", rloc);
			for (size_t k = 10; k + 16 <= sl && off < (int)sizeof line - 48; k += 16)
				off += snprintf(line + off, sizeof line - off, " %s",
						ip6_tail(e + k, t, sizeof t));
			off += snprintf(line + off, sizeof line - off, "]");
			i += 2 + sl; n++;
		}
		LOG_INF("%s (%d)", line, n);
	} else {
		LOG_WRN("child table: read failed");
	}
	len = sizeof buf;
	if (ncp_link_get(SPINEL_PROP_THREAD_ADDRESS_CACHE_TABLE, buf, &len, 2000) == 0) {
		size_t i = 0; int n = 0; int off = 0;
		off += snprintf(line + off, sizeof line - off, "addr cache:");
		while (i + 2 <= len) {
			uint16_t sl = (uint16_t)(buf[i] | (buf[i + 1] << 8));
			const uint8_t *e = buf + i + 2;
			if (i + 2 + sl > len || sl < 20) break;
			uint16_t rloc = (uint16_t)(e[16] | (e[17] << 8));
			/* state: 0 cached, 1 snooped, 2 query, 3 query-retry */
			if (off < (int)sizeof line - 64)
				off += snprintf(line + off, sizeof line - off, " %s->0x%04x/s%u",
						ip6_tail(e, t, sizeof t), rloc, e[19]);
			i += 2 + sl; n++;
		}
		LOG_INF("%s (%d)", line, n);
	} else {
		LOG_WRN("addr cache: read failed");
	}
	/* router links: id, next hop, cost, LQ in/out, age, linked */
	len = sizeof buf;
	if (ncp_link_get(SPINEL_PROP_THREAD_ROUTER_TABLE_X, buf, &len, 2000) == 0) {
		size_t i = 0; int off = 0;
		off += snprintf(line + off, sizeof line - off, "routers:");
		while (i + 2 <= len) {
			uint16_t sl = (uint16_t)(buf[i] | (buf[i + 1] << 8));
			const uint8_t *e = buf + i + 2;
			if (i + 2 + sl > len || sl < 17) break;
			if (off < (int)sizeof line - 64)
				off += snprintf(line + off, sizeof line - off,
						" [0x%04x id%u nh%u cost%u lq%u/%u age%u %s]",
						(unsigned)(e[8] | (e[9] << 8)), e[10], e[11], e[12],
						e[13], e[14], e[15], e[16] ? "link" : "nolink");
			i += 2 + sl;
		}
		LOG_INF("%s", line);
	}
	/* MAC counters (raw totals; diff consecutive snapshots) */
	len = sizeof buf;
	if (ncp_link_get(SPINEL_PROP_CNTR_ALL_MAC_COUNTERS_X, buf, &len, 2000) == 0 && len > 4) {
		uint32_t tx[20] = {0}, rx[20] = {0};
		size_t i = 0;
		for (int part = 0; part < 2 && i + 2 <= len; part++) {
			uint16_t sl = (uint16_t)(buf[i] | (buf[i + 1] << 8));
			uint32_t *d = part ? rx : tx;
			for (size_t k = 0; k + 4 <= sl && k / 4 < 20; k += 4) {
				const uint8_t *q = buf + i + 2 + k;
				d[k / 4] = q[0] | (q[1] << 8) | (q[2] << 16) | ((uint32_t)q[3] << 24);
			}
			i += 2 + sl;
		}
		uint32_t ipok = 0, ipfail = 0; size_t l4;
		uint8_t v[4];
		l4 = 4; if (ncp_link_get(SPINEL_PROP_CNTR_IP_TX_SUCCESS_X, v, &l4, 1000) == 0 && l4 == 4) ipok = v[0] | (v[1] << 8) | (v[2] << 16) | ((uint32_t)v[3] << 24);
		l4 = 4; if (ncp_link_get(SPINEL_PROP_CNTR_IP_TX_FAILURE_X, v, &l4, 1000) == 0 && l4 == 4) ipfail = v[0] | (v[1] << 8) | (v[2] << 16) | ((uint32_t)v[3] << 24);
		LOG_INF("mac: txUni=%u txAckReq=%u txAcked=%u txRetry=%u txCca=%u txAbort=%u "
			"txBusy=%u txDirectMaxRetry=%u txIndirectMaxRetry=%u | rxTotal=%u rxUni=%u "
			"rxDup=%u rxUnkNbr=%u rxSec=%u rxFcs=%u rxOther=%u | ipTxOk=%u ipTxFail=%u",
			tx[1], tx[3], tx[4], tx[11], tx[12], tx[13], tx[14], tx[15], tx[16],
			rx[0], rx[1], rx[10], rx[12], rx[14], rx[15], rx[16], ipok, ipfail);
	}
}


/* ── probe trigger (diagnostics) ─────────────────────────────────────
 * With NN_GW_MESH_TABLES set, a line written to /run/nn-gw-probe:
 *     <ipv6> <device_id hex> <count> <body bytes> <gap ms> <cell 0-15>
 * makes this gateway send <count> H2D frames with cmd 0x0F70|cell straight
 * (cells 8-15: RAW datagrams "NNPRB"+seq+<body> bytes, no envelope)
 * to <ipv6>, bypassing the device routing table (which only knows devices
 * whose uplink came through THIS gateway).  Devices log unknown H2D cmds
 * as "H2D received: cmd=0x0f7N", so counting those lines on the console
 * gives per-frame delivery for a chosen gateway -> device path.  Devices
 * do not verify H2D signatures here; the frame is signed with this
 * gateway's key only because the encoder requires a signer. */
#define PROBE_FILE "/run/nn-gw-probe"

static void run_probe_file(void)
{
	FILE *f = fopen(PROBE_FILE, "r");
	if (!f) return;
	char ip[64], didhex[40];
	unsigned count, size, gap, cell;
	int n = fscanf(f, "%63s %39s %u %u %u %u", ip, didhex, &count, &size, &gap, &cell);
	fclose(f);
	unlink(PROBE_FILE);
	if (n != 6) { LOG_WRN("probe: bad request"); return; }
	struct in6_addr dst;
	if (inet_pton(AF_INET6, ip, &dst) != 1) { LOG_WRN("probe: bad ipv6"); return; }
	uint8_t did[16]; size_t dl = strlen(didhex) / 2;
	if (dl == 0 || dl > sizeof did) { LOG_WRN("probe: bad device id"); return; }
	for (size_t i = 0; i < dl; i++) sscanf(didhex + 2 * i, "%2hhx", &did[i]);
	if (count > 50) count = 50;
	if (size > 400) size = 400;
	static uint8_t inner[6 + 400], frame[NN_PROTO_OVERHEAD + 16 + sizeof inner];
	LOG_INF("probe: %u x %uB body -> %s cell %u", count, size, ip, cell);
	for (unsigned i = 0; i < count; i++) {
		uint16_t cmd = 0x0F70 | (cell & 0x0F);
		inner[0] = cmd & 0xff; inner[1] = cmd >> 8;
		uint32_t tid = 0xE0000000u | i;
		memcpy(inner + 2, &tid, 4);
		memset(inner + 6, (int)(i & 0xff), size);
		int fl;
		if (cell & 8) {
			/* raw probe: no nn_proto envelope, "NNPRB" + seq + padding,
			 * so an 8-byte one fits a single 802.15.4 frame */
			memcpy(frame, "NNPRB", 5);
			memcpy(frame + 5, &i, 3);
			memset(frame + 8, 0xA5, size);
			fl = 8 + (int)size;
		} else {
			fl = nn_proto_encode(NN_PROTO_TYPE_H2D, did, (uint16_t)dl,
					     inner, 6 + size, gw_identity_sign, NULL, frame, sizeof frame);
		}
		int rv = fl > 0 ? proto_udp_send_unicast(&dst, 49190 /* PROTO_ROUTER_UDP_PORT */, frame, (size_t)fl) : fl;
		if (rv < 0) LOG_WRN("probe %u: send rv=%d", i, rv);
		struct timespec ts = { gap / 1000, (long)(gap % 1000) * 1000000L };
		nanosleep(&ts, NULL);
	}
	LOG_INF("probe: done");
}


/* ── channel tools (diagnostics / operator) ─────────────────────────
 * With NN_GW_MESH_TABLES set, a line written to /run/nn-gw-cmd runs:
 *   energy-scan <ms per channel>   max RSSI on channels 11..26, logged
 *   active-dataset                 logs the NCP's active dataset TLVs (hex)
 *   set-channel <11..26> <delay s> MGMT_PENDING_SET to the leader: the
 *                                  current active dataset with the new
 *                                  channel, a newer active timestamp, a
 *                                  pending timestamp and a delay timer --
 *                                  the whole mesh switches together when
 *                                  the timer expires (Thread standard). */
#define CMD_FILE "/run/nn-gw-cmd"
#define SP_MAC_SCAN_STATE      0x30
#define SP_MAC_SCAN_MASK       0x31
#define SP_MAC_SCAN_PERIOD     0x32
#define SP_MAC_ENERGY_SCAN_RES 0x39
#define SP_ACTIVE_DATASET_TLVS 0x153C
#define SP_MGMT_SET_PENDING_TLVS 0x153E

static volatile int8_t g_ed_max[27];
static volatile int g_ed_seen, g_scan_done;

static void on_prop_watch(uint32_t prop, const uint8_t *v, size_t len)
{
	if (prop == SP_MAC_ENERGY_SCAN_RES && len >= 2 && v[0] >= 11 && v[0] <= 26) {
		g_ed_max[v[0]] = (int8_t)v[1];
		g_ed_seen++;
	} else if (prop == SP_MAC_SCAN_STATE && len >= 1 && v[0] == 0) {
		g_scan_done = 1;
	} else if (prop == SP_MGMT_SET_PENDING_TLVS) {
		LOG_INF("set-channel: MGMT_SET_PENDING result len=%zu status=%u", len, len ? v[0] : 0);
	}
}

/* Energy scan of channels 11..26; out[i] = max dBm on channel 11+i (127 = no
 * result).  Returns 0, or -EIO when the NCP reported nothing.  One at a
 * time (the bench command file and hub requests share the NCP). */
static pthread_mutex_t g_scan_lock = PTHREAD_MUTEX_INITIALIZER;

static int gw_energy_scan(unsigned period, int8_t out[16])
{
	if (period < 50) period = 50;
	if (period > 2000) period = 2000;
	pthread_mutex_lock(&g_scan_lock);
	uint8_t mask[16];
	for (int i = 0; i < 16; i++) { mask[i] = (uint8_t)(11 + i); g_ed_max[11 + i] = 127; }
	g_ed_seen = 0; g_scan_done = 0;
	uint32_t ls = 0;
	(void)ncp_link_set_raw(SP_MAC_SCAN_MASK, mask, sizeof mask, &ls, 2000);
	uint8_t per[2] = { (uint8_t)(period & 0xff), (uint8_t)(period >> 8) };
	(void)ncp_link_set_raw(SP_MAC_SCAN_PERIOD, per, 2, &ls, 2000);
	uint8_t st = 2;   /* SPINEL_SCAN_STATE_ENERGY */
	int rv = ncp_link_set_raw(SP_MAC_SCAN_STATE, &st, 1, &ls, 2000);
	LOG_INF("energy-scan: start rv=%d ls=%u (%u ms/channel)", rv, ls, period);
	for (int t = 0; t < 16 * (int)period / 100 + 50 && !g_scan_done; t++) {
		struct timespec ts = { 0, 100 * 1000000L };
		nanosleep(&ts, NULL);
	}
	char line[256]; int off = 0;
	for (int c = 11; c <= 26; c++) {
		out[c - 11] = g_ed_max[c];
		off += snprintf(line + off, sizeof line - off, " %d:%d", c, g_ed_max[c]);
	}
	int seen = g_ed_seen, done = g_scan_done;
	pthread_mutex_unlock(&g_scan_lock);
	LOG_INF("energy-scan: done=%d results=%d max-dBm per channel:%s", done, seen, line);
	return (rv == 0 && seen > 0) ? 0 : -EIO;
}

static void cmd_energy_scan(unsigned period)
{
	int8_t out[16];
	(void)gw_energy_scan(period, out);
}

static int gw_set_channel(unsigned ch, unsigned delay_s)
{
	if (ch < 11 || ch > 26) { LOG_WRN("set-channel: bad channel %u", ch); return -EINVAL; }
	if (delay_s < 30) delay_s = 30;     /* sleepy/slow nodes need time to hear it */
	uint8_t act[256]; size_t al = sizeof act;
	if (ncp_link_get(SP_ACTIVE_DATASET_TLVS, act, &al, 3000) != 0 || al == 0) {
		LOG_WRN("set-channel: cannot read the active dataset"); return -EIO;
	}
	uint8_t out[300]; size_t o = 0, i = 0;
	int have_ch = 0, have_ts = 0;
	uint64_t now = (uint64_t)time(NULL);
	while (i + 2 <= al) {
		uint8_t t = act[i], l = act[i + 1];
		if (i + 2 + l > al) break;
		const uint8_t *v = act + i + 2;
		if (t == 51 || t == 52) { i += 2 + l; continue; }        /* drop old pending/delay */
		out[o++] = t; out[o++] = l; memcpy(out + o, v, l);
		if (t == 0 && l == 3) {                                   /* Channel: page, u16 BE */
			out[o + 1] = (uint8_t)(ch >> 8); out[o + 2] = (uint8_t)ch; have_ch = 1;
		}
		if (t == 14 && l == 8) {                                  /* Active Timestamp */
			uint64_t sec = 0;
			for (int k = 0; k < 6; k++) sec = (sec << 8) | v[k];
			sec = (sec + 1 > now) ? sec + 1 : now;              /* strictly newer */
			for (int k = 5; k >= 0; k--) { out[o + k] = (uint8_t)sec; sec >>= 8; }
			have_ts = 1;
		}
		o += l; i += 2 + l;
	}
	if (!have_ch || !have_ts) { LOG_WRN("set-channel: active dataset lacks channel/timestamp"); return -EPROTO; }
	out[o++] = 51; out[o++] = 8;                                      /* Pending Timestamp */
	uint64_t pts = now;
	for (int k = 5; k >= 0; k--) { out[o + k] = (uint8_t)pts; pts >>= 8; }
	out[o + 6] = 0; out[o + 7] = 0; o += 8;
	uint32_t ms = delay_s * 1000u;
	out[o++] = 52; out[o++] = 4;                                      /* Delay Timer (ms, BE) */
	out[o++] = (uint8_t)(ms >> 24); out[o++] = (uint8_t)(ms >> 16); out[o++] = (uint8_t)(ms >> 8); out[o++] = (uint8_t)ms;
	uint32_t ls = 0;
	int rv = ncp_link_set_raw(SP_MGMT_SET_PENDING_TLVS, out, o, &ls, 5000);
	LOG_INF("set-channel: MGMT_PENDING_SET channel %u in %u s -> rv=%d ls=%u (%zu B)", ch, delay_s, rv, ls, o);
	return rv ? -EIO : (ls ? -(int)ls : 0);
}

static void cmd_set_channel(unsigned ch, unsigned delay_s) { (void)gw_set_channel(ch, delay_s); }

static void run_cmd_file(void)
{
	FILE *f = fopen(CMD_FILE, "r");
	if (!f) return;
	char cmd[32]; unsigned a = 0, b = 0;
	int n = fscanf(f, "%31s %u %u", cmd, &a, &b);
	fclose(f);
	unlink(CMD_FILE);
	if (n >= 2 && !strcmp(cmd, "energy-scan")) cmd_energy_scan(a ? a : 200);
	else if (n >= 3 && !strcmp(cmd, "set-channel")) cmd_set_channel(a, b);
	else if (n >= 1 && !strcmp(cmd, "active-dataset")) {
		uint8_t act[256]; size_t al = sizeof act;
		if (ncp_link_get(SP_ACTIVE_DATASET_TLVS, act, &al, 3000) == 0) {
			char hex[520]; for (size_t i = 0; i < al && i < 255; i++) snprintf(hex + 2 * i, 3, "%02x", act[i]);
			LOG_INF("active-dataset: %zu B %s", al, hex);
		} else LOG_WRN("active-dataset: read failed");
	}
	else LOG_WRN("cmd: unknown '%s'", n > 0 ? cmd : "");
}

/* ── systemd watchdog ───────────────────────────────────────────────
 * With WatchdogSec= in the unit, systemd restarts a gateway that stops
 * pinging.  A frozen process (SIGSTOP, deadlock) or a main loop stuck for
 * 30 s stops the pings; startup (dataset apply can take tens of seconds)
 * is covered until the loop runs.  Measured 2026-09-24: a frozen host
 * process left its NCP routing the mesh's uplink into nothing. */
#include <systemd/sd-daemon.h>
static volatile time_t g_loop_tick;          /* 0 = still starting up */

static void *watchdog_thread(void *arg)
{
	(void)arg;
	for (;;) {
		time_t now = time(NULL);
		if (g_loop_tick == 0 || now - g_loop_tick < 30)
			sd_notify(0, "WATCHDOG=1");
		sleep(10);
	}
	return NULL;
}

static void watchdog_start(void)
{
	uint64_t usec = 0;
	if (sd_watchdog_enabled(0, &usec) <= 0) return;      /* unit has no WatchdogSec */
	pthread_t th;
	if (pthread_create(&th, NULL, watchdog_thread, NULL) == 0) {
		pthread_detach(th);
		LOG_INF("systemd watchdog: %llu s", (unsigned long long)(usec / 1000000));
	}
}

/* ── hub -> gateway commands (NN_PROTO_TYPE_H2G) ───────────────────
 * Signed by the hub, checked against the hub public key from
 * provisioning, fresh (timestamp within 5 min) and never run twice (tid
 * ring) -- a channel change stops the whole mesh for a while, so a
 * forged or replayed request must not work.  Work runs on its own
 * thread: a scan takes ~16 x the dwell and the TCP rx path must not
 * block.  Replies are D2G [cmd+1][tid][status][body]. */
#define H2G_MAX_AGE_MS  (5 * 60 * 1000LL)
struct h2g_job { uint16_t cmd; uint32_t tid; uint8_t body[64]; size_t body_len; };
static struct h2g_job g_h2g_q[4];
static int g_h2g_head, g_h2g_n;
static uint32_t g_h2g_seen[16];
static int g_h2g_seen_i;
static pthread_mutex_t g_h2g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_h2g_cv   = PTHREAD_COND_INITIALIZER;

static void h2g_reply(uint16_t cmd, uint32_t tid, const uint8_t *body, size_t blen)
{
	uint8_t inner[2 + 4 + 300];
	if (blen > 300) blen = 300;
	inner[0] = (uint8_t)cmd; inner[1] = (uint8_t)(cmd >> 8);
	inner[2] = (uint8_t)tid; inner[3] = (uint8_t)(tid >> 8);
	inner[4] = (uint8_t)(tid >> 16); inner[5] = (uint8_t)(tid >> 24);
	memcpy(inner + 6, body, blen);
	uint8_t f[NN_PROTO_OVERHEAD + 8 + sizeof inner];
	int n = nn_proto_encode(NN_PROTO_TYPE_D2G, gw_identity_get_id(), 8,
				inner, 6 + blen, gw_identity_sign, NULL, f, sizeof f);
	if (n > 0) (void)proto_tcp_enqueue(f, (size_t)n);
}

static void *h2g_worker(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&g_h2g_lock);
		while (g_h2g_n == 0) pthread_cond_wait(&g_h2g_cv, &g_h2g_lock);
		struct h2g_job j = g_h2g_q[g_h2g_head];
		g_h2g_head = (g_h2g_head + 1) % 4; g_h2g_n--;
		pthread_mutex_unlock(&g_h2g_lock);

		uint8_t out[2 + 256];
		if (j.cmd == NN_PROTO_CMD_GW_CHANNEL_SCAN) {
			unsigned per = j.body_len >= 2 ? (unsigned)(j.body[0] | (j.body[1] << 8)) : 200;
			uint8_t ch = 0; size_t cl = 1;
			(void)ncp_link_get(SPINEL_PROP_PHY_CHAN, &ch, &cl, 1000);
			int rv = gw_energy_scan(per, (int8_t *)(out + 2));
			out[0] = (uint8_t)(int8_t)rv; out[1] = ch;
			h2g_reply(NN_PROTO_CMD_GW_CHANNEL_SCAN_RESULT, j.tid, out, 18);
		} else if (j.cmd == NN_PROTO_CMD_GW_CHANNEL_SET && j.body_len >= 3) {
			int rv = gw_set_channel(j.body[0], (unsigned)(j.body[1] | (j.body[2] << 8)));
			out[0] = (uint8_t)(int8_t)rv;
			h2g_reply(NN_PROTO_CMD_GW_CHANNEL_SET_RESULT, j.tid, out, 1);
		} else if (j.cmd == NN_PROTO_CMD_GW_ROUTE_SET && j.body_len >= 2) {
			uint16_t dsz = (uint16_t)(j.body[0] | (j.body[1] << 8));
			int rv = -EINVAL;
			if (dsz > 0 && (size_t)2 + dsz + 16 <= j.body_len) {
				struct in6_addr a;
				memcpy(&a, j.body + 2 + dsz, 16);
				rv = proto_router_remember(j.body + 2, dsz, &a);
				char as[INET6_ADDRSTRLEN] = "?";
				inet_ntop(AF_INET6, &a, as, sizeof as);
				LOG_INF("H2G route-set: device -> %s rv=%d", as, rv);
			}
			out[0] = (uint8_t)(int8_t)rv;
			h2g_reply(NN_PROTO_CMD_GW_ROUTE_SET_RESULT, j.tid, out, 1);
		} else if (j.cmd == NN_PROTO_CMD_GW_DATASET_GET) {
			size_t al = 255;
			int rv = ncp_link_get(SP_ACTIVE_DATASET_TLVS, out + 1, &al, 3000);
			out[0] = (uint8_t)(int8_t)(rv ? -EIO : 0);
			h2g_reply(NN_PROTO_CMD_GW_DATASET, j.tid, out, rv ? 1 : 1 + al);
		} else {
			out[0] = (uint8_t)(int8_t)-ENOTSUP;
			h2g_reply((uint16_t)(j.cmd + 1), j.tid, out, 1);
		}
	}
	return NULL;
}

/* An H2D frame we could not forward: tell the hub (D2G), which re-sends it
 * through another gateway instead of waiting for the device to time out. */
static void on_h2d_fail(const uint8_t *did, uint16_t dsz, const uint8_t *payload,
			size_t plen, int reason)
{
	if (dsz > 32 || plen < 6) return;
	uint8_t inner[2 + 1 + 2 + 32 + 6];
	size_t o = 0;
	inner[o++] = (uint8_t)NN_PROTO_CMD_GW_H2D_UNDELIVERABLE;
	inner[o++] = (uint8_t)(NN_PROTO_CMD_GW_H2D_UNDELIVERABLE >> 8);
	inner[o++] = (uint8_t)(int8_t)reason;
	inner[o++] = (uint8_t)dsz; inner[o++] = (uint8_t)(dsz >> 8);
	memcpy(inner + o, did, dsz); o += dsz;
	memcpy(inner + o, payload, 6); o += 6;            /* cmd + tid */
	uint8_t f[NN_PROTO_OVERHEAD + 8 + sizeof inner];
	int n = nn_proto_encode(NN_PROTO_TYPE_D2G, gw_identity_get_id(), 8,
				inner, o, gw_identity_sign, NULL, f, sizeof f);
	if (n > 0) (void)proto_tcp_enqueue(f, (size_t)n);
}

static void on_h2g(const uint8_t *frame, size_t len)
{
	struct nn_proto_view v;
	if (nn_proto_parse(frame, len, &v, NULL) != 0) return;
	if (v.device_id_size != 8 || memcmp(v.device_id, gw_identity_get_id(), 8) != 0) {
		LOG_WRN("H2G: not addressed to this gateway, dropped");
		return;
	}
	const uint8_t *hub_pub = gw_provision_get_hub_pubkey();
	if (!hub_pub || nn_proto_verify_sig(&v, gw_identity_verify, (void *)hub_pub) != 0) {
		LOG_WRN("H2G: bad hub signature, dropped");
		return;
	}
	if (v.payload_size < 14) return;
	const uint8_t *p = v.payload;
	uint16_t cmd = (uint16_t)(p[0] | (p[1] << 8));
	uint32_t tid = (uint32_t)p[2] | ((uint32_t)p[3] << 8) | ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 24);
	uint64_t ts = 0;
	for (int k = 7; k >= 0; k--) ts = (ts << 8) | p[6 + k];
	struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
	long long age = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000 - (long long)ts;
	if (age > H2G_MAX_AGE_MS || age < -H2G_MAX_AGE_MS) {
		LOG_WRN("H2G cmd=0x%04x tid=%u: stale (age %lld ms), dropped", cmd, tid, age);
		return;
	}
	pthread_mutex_lock(&g_h2g_lock);
	for (int i = 0; i < 16; i++) {
		if (g_h2g_seen[i] == tid && tid) {
			pthread_mutex_unlock(&g_h2g_lock);
			LOG_INF("H2G cmd=0x%04x tid=%u: already handled, dropped", cmd, tid);
			return;
		}
	}
	if (g_h2g_n == 4) {
		pthread_mutex_unlock(&g_h2g_lock);
		uint8_t st = (uint8_t)(int8_t)-EBUSY;
		h2g_reply((uint16_t)(cmd + 1), tid, &st, 1);
		return;
	}
	g_h2g_seen[g_h2g_seen_i] = tid; g_h2g_seen_i = (g_h2g_seen_i + 1) % 16;
	struct h2g_job *j = &g_h2g_q[(g_h2g_head + g_h2g_n) % 4];
	j->cmd = cmd; j->tid = tid;
	j->body_len = v.payload_size - 14 > sizeof j->body ? sizeof j->body : v.payload_size - 14;
	memcpy(j->body, p + 14, j->body_len);
	g_h2g_n++;
	pthread_cond_signal(&g_h2g_cv);
	pthread_mutex_unlock(&g_h2g_lock);
	LOG_INF("H2G cmd=0x%04x tid=%u accepted", cmd, tid);
}

/* ── operate-mode RX adapters ───────────────────────────────────────
 *
 * proto_router's entry points return int and have narrower argument
 * lists than what proto_udp / proto_tcp's on_rx function pointers
 * expect.  Two tiny adapters bridge the shapes. */

static void operate_on_udp_rx(const uint8_t *frame, size_t len,
			      const struct in6_addr *src,
			      uint16_t src_port, void *user)
{
	(void)src_port; (void)user;
	(void)proto_router_on_udp_rx(frame, len, src);
}

static void operate_on_tcp_rx(const uint8_t *frame, size_t len, void *user)
{
	(void)user;
	(void)proto_router_on_tcp_rx(frame, len);
}

/* ── BLE provisioning daemon ───────────────────────────────────────── */

static int do_provision(const char *adapter)
{
	LOG_INF("starting BLE provisioning backend (adapter='%s')",
		adapter ? adapter : "/org/bluez/hci0");

	int rc = gw_identity_init();
	if (rc) {
		LOG_ERR("gw_identity_init: %d", rc);
		return 31;
	}
	rc = gw_provision_init();
	if (rc) {
		LOG_WRN("gw_provision_init: %d (continuing)", rc);
	}

	rc = gw_ble_prov_linux_start(adapter);
	if (rc) {
		LOG_ERR("gw_ble_prov_linux_start: %d", rc);
		return 32;
	}
	rc = gw_ble_prov_linux_run();
	gw_ble_prov_linux_stop();
	return rc < 0 ? 33 : 0;
}

int main(int argc, char **argv)
{
	const char *cmd = (argc > 1) ? argv[1] : "spinel";

	if (!strcmp(cmd, "spinel")) {
		char pbuf[128];
		const char *port = resolve_ncp_port((argc > 2) ? argv[2] : NULL,
						    pbuf, sizeof pbuf);
		if (!port) return 60;
		return do_spinel(port);
	}
	if (!strcmp(cmd, "crypto")) {
		return do_crypto();
	}
	if (!strcmp(cmd, "identity")) {
		return do_identity();
	}
	if (!strcmp(cmd, "identity-import")) {
		const char *hex = (argc > 2) ? argv[2] : NULL;
		bool force = (argc > 3) && !strcmp(argv[3], "--force");
		return do_identity_import(hex, force);
	}
	if (!strcmp(cmd, "provision")) {
		const char *adapter = (argc > 2) ? argv[2] : NULL;
		return do_provision(adapter);
	}
	if (!strcmp(cmd, "ncp")) {
		char pbuf[128];
		const char *uart = resolve_ncp_port((argc > 2) ? argv[2] : NULL,
						    pbuf, sizeof pbuf);
		if (!uart) return 60;
		LOG_INF("ncp smoke test on %s", uart);
		int rc = ncp_link_init_linux(uart);
		if (rc) { LOG_ERR("ncp_link_init_linux: %d", rc); return 61; }
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 300 * 1000 * 1000 };
		nanosleep(&ts, NULL);

		const uint32_t probes[] = {
			SPINEL_PROP_NCP_VERSION,
			SPINEL_PROP_PROTOCOL_VERSION,
			SPINEL_PROP_INTERFACE_TYPE,
			SPINEL_PROP_CAPS,
		};
		const char *labels[] = {
			"NCP_VERSION", "PROTOCOL_VERSION", "INTERFACE_TYPE",
			"CAPS",
		};
		for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
			uint8_t buf[256];
			size_t len = sizeof buf;
			int rv = ncp_link_get(probes[i], buf, &len, 2000);
			if (rv < 0) {
				LOG_WRN("  %s: rv=%d", labels[i], rv);
				continue;
			}
			fprintf(stderr, "  %-18s len=%zu  ", labels[i], len);
			for (size_t j = 0; j < len && j < 32; j++) {
				fprintf(stderr, "%02x", buf[j]);
			}
			fprintf(stderr, "%s\n", len > 32 ? "..." : "");
		}
		LOG_INF("ncp smoke test done");
		return 0;
	}
	if (!strcmp(cmd, "operate")) {
		char pbuf[128];
		const char *uart = resolve_ncp_port((argc > 2) ? argv[2] : NULL,
						    pbuf, sizeof pbuf);
		if (!uart) return 60;
		LOG_INF("starting operate mode (NCP=%s)", uart);

		watchdog_start();
		/* 1. identity + persisted provision ----------------------- */
		int rc = gw_identity_init();
		if (rc) { LOG_ERR("gw_identity_init: %d", rc); return 51; }
		rc = gw_provision_init();
		if (rc) { LOG_ERR("gw_provision_init: %d", rc); return 52; }
		if (!gw_provision_is_complete()) {
			LOG_ERR("not provisioned — run provision-net first");
			return 53;
		}

		/* 2. NCP link up + apply OT dataset + bring Thread up ---- */
		rc = ncp_link_init_linux(uart);
		if (rc) { LOG_ERR("ncp_link_init_linux: %d", rc); return 54; }
		struct timespec settle = { .tv_sec = 0, .tv_nsec = 500*1000*1000 };
		nanosleep(&settle, NULL);

		size_t ds_len = 0;
		const uint8_t *ds = gw_provision_get_ot_dataset(&ds_len);
		if (!ds || ds_len == 0) {
			LOG_ERR("no OT dataset in provision blob");
			return 55;
		}
		rc = gw_ot_apply_dataset(ds, ds_len);
		if (rc) { LOG_ERR("gw_ot_apply_dataset: %d", rc); return 56; }
		rc = gw_ot_bring_up();
		if (rc) { LOG_ERR("gw_ot_bring_up: %d", rc); return 57; }

		/* Wait for the mesh-local prefix / role to settle.  Poll
		 * NET_ROLE every 250 ms up to ~10 s. */
		LOG_INF("waiting for Thread role…");
		uint8_t role = 0;
		for (int i = 0; i < 40; i++) {
			uint8_t b = 0; size_t bl = 1;
			if (ncp_link_get(SPINEL_PROP_NET_ROLE, &b, &bl, 500) == 0
			    && bl >= 1) {
				role = b;
				if (role != 0) break;  /* 0 = detached */
			}
			struct timespec t = { .tv_sec = 0, .tv_nsec = 250*1000*1000 };
			nanosleep(&t, NULL);
		}
		LOG_INF("Thread NET_ROLE=%u", role);

		/* 3. proto_router + proto_udp (netif-bypass) ------------- */
		rc = proto_router_init();
		if (rc) { LOG_ERR("proto_router_init: %d", rc); return 58; }

		struct proto_udp_config ucfg = {
			.port       = 49190,  /* matches sensor's NODE_MGR_NN_PROTO_PORT */
			.on_rx      = operate_on_udp_rx,
			.on_rx_user = NULL,
		};
		rc = proto_udp_init(&ucfg);
		if (rc) { LOG_ERR("proto_udp_init: %d", rc); return 59; }

		/* 4. proto_tcp out to hub -------------------------------- */
		const char *hub = gw_provision_get_hub_mdns();
		LOG_INF("hub target: %s:8767", hub);
		struct proto_tcp_config tcfg = {
			.hub_hostname = hub,
			.hub_port     = 8767,
			.on_rx        = operate_on_tcp_rx,
			.on_rx_user   = NULL,
		};
		rc = proto_tcp_init(&tcfg);
		if (rc) { LOG_ERR("proto_tcp_init: %d", rc); return 60; }

		/* 5. heartbeat loop ─────────────────────────────────────
		 *   every 5 s:  D2G HUB_STATUS_QUERY to hub via TCP
		 *               G2D GATEWAY_HELLO mcast on Thread so
		 *               sensors learn our mesh-local IPv6
		 */
		const uint8_t *gw_id = gw_identity_get_id();

		uint8_t ml_eid[16] = {0};
		{
			uint8_t buf[16]; size_t bl = sizeof buf;
			if (ncp_link_get(SPINEL_PROP_IPV6_ML_ADDR,
					 buf, &bl, 1000) == 0 && bl >= 16) {
				memcpy(ml_eid, buf, 16);
			}
		}
		ncp_link_set_prop_watch_cb(on_prop_watch);
		{
			pthread_t th;
			if (pthread_create(&th, NULL, h2g_worker, NULL) == 0) {
				pthread_detach(th);
				proto_router_set_h2g_handler(on_h2g);
				proto_router_set_h2d_fail_handler(on_h2d_fail);
			} else LOG_ERR("h2g worker: pthread_create failed");
		}
		arm_stop_signals();
		while (!g_stop) {
			g_loop_tick = time(NULL);
			sleep(5);   /* a stop signal cuts the sleep short */
			if (g_stop) break;
			{
				static unsigned tick;
				if (getenv("NN_GW_MESH_TABLES") && (tick++ % 6) == 0)
					log_mesh_tables();
				if (getenv("NN_GW_MESH_TABLES")) {
					run_probe_file();
					run_cmd_file();
				}
			}

			/* G2D GATEWAY_HELLO mcast — payload:
			 * [2B cmd LE][16B gw mesh-local IPv6][2B interval LE]
			 * [1B online].  No device_id (mcast). */
			/* + [2B RLOC16 LE]: sensors pick the NEAREST gateway by
			 * mesh path cost to it (node_mgr gw_policy.c); older
			 * sensors read only the first 19 bytes of the body. */
			uint8_t hello_inner[2 + 16 + 2 + 1 + 2] = {0};
			hello_inner[0] = (uint8_t)(NN_PROTO_CMD_GATEWAY_HELLO & 0xff);
			hello_inner[1] = (uint8_t)(NN_PROTO_CMD_GATEWAY_HELLO >> 8);
			memcpy(hello_inner + 2, ml_eid, 16);
			hello_inner[18] = 5;        /* 5 s interval, LE */
			hello_inner[19] = 0;
			hello_inner[20] = (proto_tcp_get_state() == PROTO_TCP_UP) ? 1 : 0;
			{
				uint8_t rb[2] = {0xFE, 0xFF}; size_t rl = 2;   /* 0xFFFE = unknown */
				if (ncp_link_get(SPINEL_PROP_THREAD_RLOC16, rb, &rl, 500) != 0 || rl < 2) {
					rb[0] = 0xFE; rb[1] = 0xFF;
				}
				hello_inner[21] = rb[0];
				hello_inner[22] = rb[1];
			}
			{
				uint8_t hf[NN_PROTO_OVERHEAD + sizeof hello_inner];
				int n = nn_proto_encode(NN_PROTO_TYPE_G2D,
							NULL, 0,
							hello_inner, sizeof hello_inner,
							gw_identity_sign, NULL,
							hf, sizeof hf);
				if (n > 0) (void)proto_udp_send_mcast(hf, (size_t)n);
			}

			/* D2G HUB_STATUS_QUERY → hub */
			if (proto_tcp_get_state() != PROTO_TCP_UP) continue;
			uint8_t q_inner[2] = {
				(uint8_t)(NN_PROTO_CMD_HUB_STATUS_QUERY & 0xff),
				(uint8_t)(NN_PROTO_CMD_HUB_STATUS_QUERY >> 8),
			};
			uint8_t qf[NN_PROTO_OVERHEAD + 8 + sizeof q_inner];
			int qn = nn_proto_encode(NN_PROTO_TYPE_D2G,
						 gw_id, 8,
						 q_inner, sizeof q_inner,
						 gw_identity_sign, NULL,
						 qf, sizeof qf);
			if (qn > 0) (void)proto_tcp_enqueue(qf, (size_t)qn);

			/* D2G GATEWAY_THREAD_STATE → hub.  Payload:
			 * [2B cmd LE][1B role][2B rloc16 LE][16B mleid]
			 *
			 * Without this frame the hub never learns the gateway's
			 * actual Thread role and always reports role_name='detached'
			 * for the gateway. */
			{
				uint8_t  role_b   = 0;
				size_t   role_bl  = 1;
				uint16_t rloc16   = 0;
				uint8_t  rloc16_b[2] = {0};
				size_t   rloc16_bl   = 2;
				(void)ncp_link_get(SPINEL_PROP_NET_ROLE,
						   &role_b, &role_bl, 500);
				if (ncp_link_get(SPINEL_PROP_THREAD_RLOC16,
						 rloc16_b, &rloc16_bl, 500) == 0
				    && rloc16_bl >= 2) {
					rloc16 = (uint16_t)rloc16_b[0]
					       | ((uint16_t)rloc16_b[1] << 8);
				}

				uint8_t ts_inner[2 + 1 + 2 + 16] = {0};
				ts_inner[0] = (uint8_t)(NN_PROTO_CMD_GATEWAY_THREAD_STATE & 0xff);
				ts_inner[1] = (uint8_t)(NN_PROTO_CMD_GATEWAY_THREAD_STATE >> 8);
				ts_inner[2] = role_b;
				ts_inner[3] = (uint8_t)(rloc16 & 0xff);
				ts_inner[4] = (uint8_t)(rloc16 >> 8);
				memcpy(ts_inner + 5, ml_eid, 16);

				uint8_t tf[NN_PROTO_OVERHEAD + 8 + sizeof ts_inner];
				int tn = nn_proto_encode(NN_PROTO_TYPE_D2G,
							 gw_id, 8,
							 ts_inner, sizeof ts_inner,
							 gw_identity_sign, NULL,
							 tf, sizeof tf);
				if (tn > 0) (void)proto_tcp_enqueue(tf, (size_t)tn);
			}

			/* D2G DEVICE_THREAD_STATE → hub, one per cached device.
			 * Lets the hub self-heal each device's ml_eid after sensor
			 * reboot (random IID isn't persisted on current NVS backend).
			 * Payload per frame: [2B cmd LE][2B did_size LE][N did][16B mleid] */
			{
				struct proto_router_entry snap[PROTO_ROUTER_TABLE_SIZE];
				int got = proto_router_snapshot(snap, PROTO_ROUTER_TABLE_SIZE);
				for (int i = 0; i < got; i++) {
					uint16_t ds = snap[i].did_size;
					if (ds == 0 || ds > PROTO_ROUTER_DEVICE_ID_MAX) continue;
					uint8_t dts_inner[2 + 2 + PROTO_ROUTER_DEVICE_ID_MAX + 16];
					size_t  off = 0;
					dts_inner[off++] = (uint8_t)(NN_PROTO_CMD_DEVICE_THREAD_STATE & 0xff);
					dts_inner[off++] = (uint8_t)(NN_PROTO_CMD_DEVICE_THREAD_STATE >> 8);
					dts_inner[off++] = (uint8_t)(ds & 0xff);
					dts_inner[off++] = (uint8_t)(ds >> 8);
					memcpy(dts_inner + off, snap[i].device_id, ds);
					off += ds;
					memcpy(dts_inner + off, &snap[i].addr, 16);
					off += 16;

					uint8_t df[NN_PROTO_OVERHEAD + 8 + sizeof dts_inner];
					int dn = nn_proto_encode(NN_PROTO_TYPE_D2G,
								 gw_id, 8,
								 dts_inner, off,
								 gw_identity_sign, NULL,
								 df, sizeof df);
					if (dn > 0) (void)proto_tcp_enqueue(df, (size_t)dn);
				}
			}
		}
		LOG_INF("stop requested -- taking the Thread stack down");
		(void)gw_ot_bring_down();
		return 0;
	}
	if (!strcmp(cmd, "provision-net")) {
		uint16_t port = 0;
		int mdns = 1;
		for (int i = 2; i < argc; i++) {
			if (!strcmp(argv[i], "--no-mdns")) {
				mdns = 0;
			} else {
				port = (uint16_t)atoi(argv[i]);
			}
		}
		LOG_INF("starting net provisioning backend (port=%u mdns=%d)",
			port ? port : GW_NET_PROV_DEFAULT_PORT, mdns);
		int rc = gw_identity_init();
		if (rc) { LOG_ERR("gw_identity_init: %d", rc); return 41; }
		rc = gw_provision_init();
		if (rc) LOG_WRN("gw_provision_init: %d (continuing)", rc);
		rc = gw_net_prov_start(port, mdns);
		if (rc) { LOG_ERR("gw_net_prov_start: %d", rc); return 42; }
		rc = gw_net_prov_run();
		gw_net_prov_stop();
		return rc < 0 ? 43 : 0;
	}
	fprintf(stderr,
		"usage: %s [spinel [/dev/tty...]] | [crypto] | [identity]\n"
		"       %s identity-import <64 hex | - (stdin)> [--force]\n"
		"       %s [provision [/org/bluez/hciN]]\n"
		"       %s [provision-net [port] [--no-mdns]]\n"
		"       %s [operate]\n",
		argv[0], argv[0], argv[0], argv[0], argv[0]);
	return 2;
}
