/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <psa/crypto.h>

#include <nn_osal/osal.h>
#include <nn_proto/nn_proto.h>
#include <node_mgr/hub_crypto.h>
#include <node_mgr/gw_policy.h>
#include <nn_proto_identity/nn_proto_identity.h>
#include <node_mgr/nn_proto_client.h>

NN_OSAL_LOG_MODULE(nn_proto_client);

/* ── tunables ──────────────────────────────────────────────────────────── */

#define UDP_PORT          CONFIG_NODE_MGR_NN_PROTO_PORT
/* Big enough for the 1280B RX buffer + field_relay's 2×768B envelope
 * scratch buffers + PSA crypto work area used by hub_crypto_decrypt for
 * X25519 ECDH + HKDF + AES-GCM (~1.5KB).  4096 was the previous default
 * and overflowed on the first H2D FIELD_OP. */
#define WORKER_STACK_SIZE 8192
#define WORKER_PRIO       6
#define RX_BUF_SIZE       1280

/* ── state ─────────────────────────────────────────────────────────────── */

static struct {
	struct nn_proto_client_config cfg;

	/* Identity (managed locally — we own NVS persistence). */
	uint8_t  priv[NN_PROTO_IDENTITY_PRIV_LEN];
	uint8_t  pub[NN_PROTO_IDENTITY_PUBKEY_LEN];
	uint8_t  device_id[NN_PROTO_CLIENT_DEVICE_ID_LEN];
	bool     identity_ready;

	/* Cached gateway address (learned from GATEWAY_HELLO multicast).
	 * 16 bytes IPv6, network byte order. */
	uint8_t  gateway_addr[16];
	uint64_t gateway_last_seen_ms;
	uint16_t gateway_interval_s;   /* advertise interval from ITS hello  */
	bool     gateway_hub_online;   /* hub-online flag from ITS hello     */
	bool     gateway_known;

	/* UDP socket. */
	nn_osal_socket_t fd;

	/* Monotonic uptime_ms of the last successful D2H send (any type).
	 * Read by the heartbeat module to skip a fire when other D2H
	 * traffic has already refreshed the hub's last_seen. */
	int64_t  last_d2h_send_ms;
} S;

K_THREAD_STACK_DEFINE(s_rx_stack, WORKER_STACK_SIZE);
static struct k_thread s_rx_thread;

/* ── identity (NVS via nn_osal_kv) ───────────────────────────────── */

static int dev_kv_load(const char *suffix, const uint8_t *value, size_t len,
		       void *user)
{
	ARG_UNUSED(user);
	if (strcmp(suffix, "p256_priv") == 0 &&
	    len == NN_PROTO_IDENTITY_PRIV_LEN) {
		memcpy(S.priv, value, NN_PROTO_IDENTITY_PRIV_LEN);
		NN_LOG_INF("device P-256 priv loaded from NVS");
	}
	return 0;
}

static int identity_init(void)
{
	psa_status_t pc = psa_crypto_init();
	if (pc != PSA_SUCCESS) {
		NN_LOG_ERR("psa_crypto_init: %d", pc);
		return -EIO;
	}
	(void)nn_osal_kv_init();
	int err = nn_osal_kv_register("nn_proto_dev", dev_kv_load, NULL);
	if (err) {
		NN_LOG_WRN("nn_osal_kv_register(nn_proto_dev): %d (continuing)", err);
	}

	bool have_saved = false;
	for (int i = 0; i < NN_PROTO_IDENTITY_PRIV_LEN; i++) {
		if (S.priv[i]) { have_saved = true; break; }
	}

	if (!have_saved) {
		int rv = nn_proto_identity_keygen(S.priv, S.pub);
		if (rv) return rv;
		err = nn_osal_kv_save("nn_proto_dev/p256_priv",
				      S.priv, NN_PROTO_IDENTITY_PRIV_LEN);
		if (err) {
			NN_LOG_ERR("nn_osal_kv_save: %d", err);
			return err;
		}
		NN_LOG_INF("generated fresh device P-256 keypair");
	} else {
		int rv = nn_proto_identity_pub_from_priv(S.priv, S.pub);
		if (rv) return rv;
	}

	/* device_id derivation must match the hub side
	 * (hub/main.py:569 — `device_x25519_pub.hex()[:16]`), i.e. the
	 * first 8 bytes of the X25519 public key.  The sensor's P-256
	 * key is still used for nn_proto frame signing; only the routing
	 * identifier comes from X25519 here. */
	{
		uint8_t x25519_pub[32];
		hub_crypto_get_device_x25519_pub(x25519_pub);
		memcpy(S.device_id, x25519_pub, NN_PROTO_CLIENT_DEVICE_ID_LEN);
	}

	NN_LOG_INF("device_id = %02x%02x%02x%02x%02x%02x%02x%02x",
		S.device_id[0], S.device_id[1], S.device_id[2], S.device_id[3],
		S.device_id[4], S.device_id[5], S.device_id[6], S.device_id[7]);
	S.identity_ready = true;
	return 0;
}

/* ── frame send helpers ──────────────────────────────────────────────── */

/* "Signature" for session-sealed cmds (FIELD_REPLY_S etc.): the AEAD
 * tag inside the sealed body authenticates origin, so the frame slot is
 * zero-filled instead of paying ~1 s of ECDSA on this core.  Receivers
 * do not verify D2H/H2D sigs on this deployment (mesh L2 + gateway
 * trust), so the zero block is inert. */
static int zero_sign(void *ctx, const uint8_t *msg, size_t msg_len,
		     uint8_t sig_out[64])
{
	ARG_UNUSED(ctx); ARG_UNUSED(msg); ARG_UNUSED(msg_len);
	memset(sig_out, 0, 64);
	return 0;
}

static int send_frame_opt(uint16_t type,
			  const uint8_t *device_id, uint16_t did_size,
			  const uint8_t *payload, size_t payload_len,
			  const uint8_t dst[16], bool nosign);

/* Zero-signature command policy — ONE table so migrated cmds behave the
 * same on every send path.  These frames' authenticity is carried by a
 * session/group AEAD, by out-of-band integrity (OTA: manifest sha256 +
 * chunk sums + MCUboot image signature), or is irrelevant (telemetry) —
 * and no receiver in this deployment verifies frame sigs anyway.  Each
 * ECDSA sign skipped saves ~1 s on this core; during an OTA download
 * that used to be ~1 s of crypto PER BLOCK REQUEST. */
static bool cmd_zero_sig(uint16_t cmd)
{
	switch (cmd) {
	case NN_PROTO_CMD_OTA_CHECK:
	case NN_PROTO_CMD_OTA_BLOCK_REQ:
	case NN_PROTO_CMD_OTA_CHUNKSUMS_REQ:
	case NN_PROTO_CMD_OTA_PATCH_REQ:
	case NN_PROTO_CMD_OTA_READY:
	case NN_PROTO_CMD_OTA_HINT_ACK:
	case NN_PROTO_CMD_AUTO_EVENT:
	case NN_PROTO_CMD_SESS_HELLO:
	case NN_PROTO_CMD_SESS_PROBE:
	case NN_PROTO_CMD_FIELD_REPLY_S:
	case NN_PROTO_CMD_AUTO_NOTIFY_S:
		return true;
	default:
		return false;
	}
}

static int send_signed_frame(uint16_t type,
			     const uint8_t *device_id, uint16_t did_size,
			     const uint8_t *payload, size_t payload_len,
			     const uint8_t dst[16])
{
	return send_frame_opt(type, device_id, did_size,
			      payload, payload_len, dst, false);
}

static int send_frame_opt(uint16_t type,
			  const uint8_t *device_id, uint16_t did_size,
			  const uint8_t *payload, size_t payload_len,
			  const uint8_t dst[16], bool nosign)
{
	if (!S.identity_ready) {
		return -ENODEV;
	}
	if (S.fd < 0) {
		return -EBADF;
	}

	uint8_t  frame[NN_PROTO_OVERHEAD + 64 + 1024];
	if (NN_PROTO_OVERHEAD + did_size + payload_len > sizeof(frame)) {
		NN_LOG_ERR("frame too large: did=%u payload=%zu",
			did_size, payload_len);
		return -EMSGSIZE;
	}
	int n = nn_proto_encode(type, device_id, did_size,
				payload, payload_len,
				nosign ? zero_sign : nn_proto_identity_sign,
				nosign ? NULL : S.priv,
				frame, sizeof(frame));
	if (n <= 0) {
		/* DELIBERATE: use printk, NOT NN_LOG_WRN.  The coap_log
		 * backend (which forwards every LOG_* message as a D2H
		 * LOG_LINE) calls back into this same send path — so a
		 * NN_LOG_WRN here triggers another encode, which may also
		 * fail and re-fire the NN_LOG_WRN, etc.  That created an
		 * infinite log-feedback loop that CPU-saturated the
		 * sensor and drowned its UART.  printk bypasses the log
		 * subsystem entirely. */
		printk("nn_proto_client: encode rv=%d\n", n);
		return n;
	}

	nn_osal_sockaddr_in6_t sa = { .port = UDP_PORT };
	memcpy(sa.addr, dst, 16);

	int rv = nn_osal_sendto(S.fd, frame, (size_t)n, 0, &sa);
	if (rv < 0 || rv != n) {
		printk("nn_proto_client: sendto rv=%d\n", rv);
		return -EIO;
	}
	/* Successful D2H bumps the heartbeat reset point — any outbound
	 * D2H (LOG_LINE, AUTO_EVENT, FIELD_REPLY, OTA_BLOCK_REQ,
	 * HEARTBEAT itself) refreshes the hub's last_seen, so heartbeat
	 * can skip its own fire when other traffic already covered it. */
	if (type == NN_PROTO_TYPE_D2H) {
		S.last_d2h_send_ms = nn_osal_uptime_ms();
	}
	return 0;
}

int64_t nn_proto_client_last_d2h_send_ms(void)
{
	return S.last_d2h_send_ms;
}

int nn_proto_client_send_d2h(const uint8_t *payload, size_t payload_len)
{
	if (!S.gateway_known) {
		return -ENETUNREACH;
	}
	return send_signed_frame(NN_PROTO_TYPE_D2H,
				 S.device_id, NN_PROTO_CLIENT_DEVICE_ID_LEN,
				 payload, payload_len, S.gateway_addr);
}

/* ── D2H request / H2D reply framework ──────────────────────────────────
 *
 * Tracks up to NN_PROTO_CLIENT_MAX_PENDING outstanding requests by tid.
 * A request inserts a slot; the RX thread, when it sees an H2D frame
 * whose inner [cmd|tid] matches a slot, copies the body into the
 * caller's buffer and signals a semaphore.  The slot is freed by the
 * caller once it wakes up (or on timeout, by zeroing the in_use flag).
 *
 * H2D frames with tid==0 (or no matching slot) fall through to the
 * registered per-cmd dispatcher list, then to the legacy `on_h2d`
 * callback so existing field_relay-style consumers keep working.
 */

/* The synchronous request_d2h paths used to maintain their own
 * `struct pend_slot` table, semaphore, and tid counter — duplicating
 * the reliable layer's machinery.  They now ride on send_d2h_reliable
 * via the sync trampoline in request_d2h_blocking() below. */

struct h2d_handler {
	uint16_t                     cmd;
	nn_proto_client_h2d_cmd_fn   fn;
	void                        *user;
	bool                         in_use;
};
/* One slot per registered H2D command.  9 were in use when CHANNEL_SCAN_REQ
 * became the 10th -- the 9th registration failed silently (-ENOMEM) and its
 * requests fell through to the app's generic handler.  Keep headroom. */
static struct h2d_handler s_h2d_handlers[12];

int nn_proto_client_register_h2d_handler(uint16_t cmd,
					 nn_proto_client_h2d_cmd_fn handler,
					 void *user)
{
	if (!handler) return -EINVAL;
	for (int i = 0; i < (int)ARRAY_SIZE(s_h2d_handlers); i++) {
		if (s_h2d_handlers[i].in_use && s_h2d_handlers[i].cmd == cmd) {
			s_h2d_handlers[i].fn   = handler;
			s_h2d_handlers[i].user = user;
			return 0;
		}
	}
	for (int i = 0; i < (int)ARRAY_SIZE(s_h2d_handlers); i++) {
		if (!s_h2d_handlers[i].in_use) {
			s_h2d_handlers[i].in_use = true;
			s_h2d_handlers[i].cmd    = cmd;
			s_h2d_handlers[i].fn     = handler;
			s_h2d_handlers[i].user   = user;
			return 0;
		}
	}
	return -ENOMEM;
}

/* Build inner [cmd:2 LE | tid:4 LE | body] and send as D2H. */
int nn_proto_client_send_d2h_reply(uint16_t cmd, uint32_t tid,
				   const uint8_t *body, size_t body_len)
{
	if (!S.gateway_known) return -ENETUNREACH;
	uint8_t inner[1280];
	if (6 + body_len > sizeof(inner)) return -EMSGSIZE;
	nn_osal_put_le16(cmd, inner);
	nn_osal_put_le32(tid, inner + 2);
	if (body_len) memcpy(inner + 6, body, body_len);
	return send_frame_opt(NN_PROTO_TYPE_D2H,
			      S.device_id, NN_PROTO_CLIENT_DEVICE_ID_LEN,
			      inner, 6 + body_len, S.gateway_addr,
			      cmd_zero_sig(cmd));
}

/* Like send_d2h_reply but with a ZERO frame signature — for frames whose
 * authenticity is either irrelevant (SESS_HELLO salt announcements) or
 * carried by a session AEAD.  Skips ~1 s of ECDSA per send. */
int nn_proto_client_send_d2h_reply_ns(uint16_t cmd, uint32_t tid,
				      const uint8_t *body, size_t body_len)
{
	if (!S.gateway_known) return -ENETUNREACH;
	uint8_t inner[256];
	if (6 + body_len > sizeof(inner)) return -EMSGSIZE;
	nn_osal_put_le16(cmd, inner);
	nn_osal_put_le32(tid, inner + 2);
	if (body_len) memcpy(inner + 6, body, body_len);
	return send_frame_opt(NN_PROTO_TYPE_D2H,
			      S.device_id, NN_PROTO_CLIENT_DEVICE_ID_LEN,
			      inner, 6 + body_len, S.gateway_addr, true);
}

/* ── D2D peer-to-peer ─────────────────────────────────────────── */

struct d2d_handler {
	uint16_t                     cmd;
	nn_proto_client_d2d_cmd_fn   fn;
	void                        *user;
	bool                         in_use;
};
static struct d2d_handler s_d2d_handlers[4];

int nn_proto_client_register_d2d_handler(uint16_t cmd,
					 nn_proto_client_d2d_cmd_fn handler,
					 void *user)
{
	if (!handler) return -EINVAL;
	for (int i = 0; i < (int)ARRAY_SIZE(s_d2d_handlers); i++) {
		if (s_d2d_handlers[i].in_use && s_d2d_handlers[i].cmd == cmd) {
			s_d2d_handlers[i].fn   = handler;
			s_d2d_handlers[i].user = user;
			return 0;
		}
	}
	for (int i = 0; i < (int)ARRAY_SIZE(s_d2d_handlers); i++) {
		if (!s_d2d_handlers[i].in_use) {
			s_d2d_handlers[i].in_use = true;
			s_d2d_handlers[i].cmd    = cmd;
			s_d2d_handlers[i].fn     = handler;
			s_d2d_handlers[i].user   = user;
			return 0;
		}
	}
	return -ENOMEM;
}

int nn_proto_client_send_d2d(const uint8_t peer_addr[16],
			     uint16_t cmd, uint32_t tid,
			     const uint8_t *body, size_t body_len)
{
	if (!peer_addr) return -EINVAL;
	uint8_t inner[1280];
	if (6 + body_len > sizeof(inner)) return -EMSGSIZE;
	nn_osal_put_le16(cmd, inner);
	nn_osal_put_le32(tid, inner + 2);
	if (body_len) memcpy(inner + 6, body, body_len);
	return send_signed_frame(NN_PROTO_TYPE_D2D,
				 S.device_id, NN_PROTO_CLIENT_DEVICE_ID_LEN,
				 inner, 6 + body_len, peer_addr);
}

/* ── Generic reliable send (D2H + D2D) ────────────────────────────
 *
 * Asynchronous request/ack with bounded retry and adaptive timeout —
 * same shape across D2H and D2D paths.  Slot table holds in-flight
 * sends; worker thread walks deadlines.
 */

#ifndef CONFIG_NODE_MGR_NN_PROTO_RELIABLE_INFLIGHT
/* 6 in-flight slots covers a sensor's realistic concurrency: 1
 * FIELD_REPLY in transit + up to 4 AUTO_NOTIFY targets during a
 * cascade burst + 1 sync request_d2h (e.g. OTA_BLOCK_REQ).  Cut from
 * the previous 8 to free ~2.7 KB of BSS so the bumped system workqueue
 * stack fits in ESP32-C6 SRAM. */
#define CONFIG_NODE_MGR_NN_PROTO_RELIABLE_INFLIGHT 6
#endif
#ifndef CONFIG_NODE_MGR_NN_PROTO_RELIABLE_BODY_MAX
/* Must be at least as large as the FIELD_REPLY ECIES envelope —
 * matches CONFIG_NODE_MGR_FIELD_RELAY_ENV_BUF (768).  Smaller values
 * would silently truncate hub-relay replies. */
#define CONFIG_NODE_MGR_NN_PROTO_RELIABLE_BODY_MAX 768
#endif
#ifndef CONFIG_NODE_MGR_NN_PROTO_RELIABLE_FIRE_BURST
#define CONFIG_NODE_MGR_NN_PROTO_RELIABLE_FIRE_BURST 3
#endif
#ifndef CONFIG_NODE_MGR_NN_PROTO_RELIABLE_FIRE_GAP_MS
#define CONFIG_NODE_MGR_NN_PROTO_RELIABLE_FIRE_GAP_MS 30
#endif
#ifndef CONFIG_NODE_MGR_NN_PROTO_RELIABLE_ROUNDS
#define CONFIG_NODE_MGR_NN_PROTO_RELIABLE_ROUNDS 4
#endif
#ifndef CONFIG_NODE_MGR_NN_PROTO_RELIABLE_INIT_TIMEOUT_MS
#define CONFIG_NODE_MGR_NN_PROTO_RELIABLE_INIT_TIMEOUT_MS 300
#endif
#ifndef CONFIG_NODE_MGR_NN_PROTO_RELIABLE_MIN_TIMEOUT_MS
#define CONFIG_NODE_MGR_NN_PROTO_RELIABLE_MIN_TIMEOUT_MS 150
#endif
#ifndef CONFIG_NODE_MGR_NN_PROTO_RELIABLE_MAX_TIMEOUT_MS
#define CONFIG_NODE_MGR_NN_PROTO_RELIABLE_MAX_TIMEOUT_MS 4000
#endif
#ifndef CONFIG_NODE_MGR_NN_PROTO_RELIABLE_STACK_SIZE
/* Sized for the deepest call path: rel_send_one (768-byte inner +
 * 1088-byte frame on stack) → send_signed_frame → nn_proto_encode
 * (Ed25519/P-256 sign uses ~1 KB itself).  3072 overflowed
 * empirically on the first FIELD_REPLY round (Zephyr FATAL 2). */
#define CONFIG_NODE_MGR_NN_PROTO_RELIABLE_STACK_SIZE 8192
#endif

#define REL_BODY_MAX  CONFIG_NODE_MGR_NN_PROTO_RELIABLE_BODY_MAX

/* Reply bodies are smaller than request bodies: the largest expected
 * payload-carrying ACK is OTA_BLOCK at 512 B + a few bytes of header.
 * Sizing reply[] independently from body[] saves ~256 B per slot
 * (×8 slots = ~2 KB BSS), which mattered when the sysworkq stack also
 * needed to grow for the cascade path. */
#ifndef CONFIG_NODE_MGR_NN_PROTO_RELIABLE_REPLY_MAX
#define CONFIG_NODE_MGR_NN_PROTO_RELIABLE_REPLY_MAX 576
#endif

struct rel_slot {
	bool      active;
	bool      acked;
	uint16_t  frame_type;     /* NN_PROTO_TYPE_D2H or _D2D — sets matching */
	uint16_t  ack_type;       /* NN_PROTO_TYPE_H2D for D2H reqs; D2D for D2D */
	uint16_t  request_cmd;
	uint16_t  ack_cmd;
	uint8_t   peer[16];       /* D2D: target peer; D2H: copied from gateway_addr */
	uint32_t  tid;
	bool      nosign;         /* session-sealed body — zero frame sig */
	uint8_t   body[REL_BODY_MAX];
	uint16_t  body_len;
	uint8_t   reply[CONFIG_NODE_MGR_NN_PROTO_RELIABLE_REPLY_MAX];
	uint16_t  reply_len;
	uint16_t  round;          /* 0..max_rounds-1 */
	uint32_t  timeout_ms;
	int64_t   deadline_ms;
	int64_t   first_send_ms;
	nn_proto_client_reliable_cb_t cb;
	void     *user;
};

static struct rel_slot s_rel[CONFIG_NODE_MGR_NN_PROTO_RELIABLE_INFLIGHT];
K_MUTEX_DEFINE(s_rel_lock);
K_SEM_DEFINE(s_rel_wake, 0, 1);

static uint32_t s_rel_last_rtt_ms;   /* updated on each ACK; shared across slots */

static uint32_t rel_initial_timeout_ms(void)
{
	if (s_rel_last_rtt_ms == 0) {
		return CONFIG_NODE_MGR_NN_PROTO_RELIABLE_INIT_TIMEOUT_MS;
	}
	uint32_t t = s_rel_last_rtt_ms * 2;
	if (t < CONFIG_NODE_MGR_NN_PROTO_RELIABLE_MIN_TIMEOUT_MS) {
		t = CONFIG_NODE_MGR_NN_PROTO_RELIABLE_MIN_TIMEOUT_MS;
	}
	if (t > CONFIG_NODE_MGR_NN_PROTO_RELIABLE_MAX_TIMEOUT_MS) {
		t = CONFIG_NODE_MGR_NN_PROTO_RELIABLE_MAX_TIMEOUT_MS;
	}
	return t;
}

/* Build inner frame for a slot's current request_cmd + tid + body
 * and emit one signed frame to peer addr.  Used by burst_fire. */
static int rel_send_one(struct rel_slot *p)
{
	uint8_t inner[REL_BODY_MAX + 6];
	nn_osal_put_le16(p->request_cmd, inner);
	nn_osal_put_le32(p->tid, inner + 2);
	if (p->body_len) memcpy(inner + 6, p->body, p->body_len);
	return send_frame_opt(p->frame_type,
			      S.device_id, NN_PROTO_CLIENT_DEVICE_ID_LEN,
			      inner, 6 + p->body_len, p->peer, p->nosign);
}

static void rel_burst_fire(struct rel_slot *p)
{
	for (int i = 0; i < CONFIG_NODE_MGR_NN_PROTO_RELIABLE_FIRE_BURST; i++) {
		int rv = rel_send_one(p);
		if (rv && i == 0) {
			NN_LOG_WRN("rel: send_one tid=%u rv=%d", p->tid, rv);
		}
		if (i + 1 < CONFIG_NODE_MGR_NN_PROTO_RELIABLE_FIRE_BURST) {
			nn_osal_sleep_ms(CONFIG_NODE_MGR_NN_PROTO_RELIABLE_FIRE_GAP_MS);
		}
	}
}

/* Match an inbound frame against an in-flight slot.  Called from the
 * D2D dispatch path AND from the H2D inner-payload dispatcher.  Copies
 * the ACK body into the slot so the worker can deliver it to the
 * callback (used by payload-carrying ACKs like OTA_BLOCK / TIME_REPLY).
 * Returns true if a match was consumed. */
static bool rel_try_ack(uint16_t frame_type, uint16_t cmd, uint32_t tid,
			const uint8_t *body, size_t body_len)
{
	k_mutex_lock(&s_rel_lock, K_FOREVER);
	for (int i = 0; i < CONFIG_NODE_MGR_NN_PROTO_RELIABLE_INFLIGHT; i++) {
		struct rel_slot *p = &s_rel[i];
		if (p->active && !p->acked &&
		    p->ack_type == frame_type &&
		    p->ack_cmd  == cmd &&
		    p->tid      == tid) {
			p->acked = true;
			size_t copy = body_len;
			if (copy > CONFIG_NODE_MGR_NN_PROTO_RELIABLE_REPLY_MAX) {
				copy = CONFIG_NODE_MGR_NN_PROTO_RELIABLE_REPLY_MAX;
			}
			if (copy && body) memcpy(p->reply, body, copy);
			p->reply_len = (uint16_t)copy;
			k_mutex_unlock(&s_rel_lock);
			k_sem_give(&s_rel_wake);
			return true;
		}
	}
	k_mutex_unlock(&s_rel_lock);
	return false;
}

static void rel_worker_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	while (1) {
		int64_t now = nn_osal_uptime_ms();
		int64_t earliest = now + 1000;

		k_mutex_lock(&s_rel_lock, K_FOREVER);
		for (int i = 0; i < CONFIG_NODE_MGR_NN_PROTO_RELIABLE_INFLIGHT; i++) {
			struct rel_slot *p = &s_rel[i];
			if (!p->active) continue;

			if (p->acked) {
				uint32_t rtt = (uint32_t)(now - p->first_send_ms);
				if (rtt > 0 && rtt < 60000) {
					s_rel_last_rtt_ms = rtt;
				}
				if (p->round > 0) {
					NN_LOG_INF("rel tid=%u acked on round %u (rtt=%u ms)",
						   p->tid, p->round, rtt);
				}
				nn_proto_client_reliable_cb_t cb = p->cb;
				void *u = p->user;
				uint32_t tid = p->tid;
				/* Snapshot reply body so it survives unlock + cb. */
				uint8_t  reply_snap[CONFIG_NODE_MGR_NN_PROTO_RELIABLE_REPLY_MAX];
				size_t   reply_snap_len = p->reply_len;
				if (reply_snap_len) {
					memcpy(reply_snap, p->reply, reply_snap_len);
				}
				p->active = false;
				k_mutex_unlock(&s_rel_lock);
				if (cb) cb(NN_PROTO_RELIABLE_OK, tid, rtt,
					   reply_snap_len ? reply_snap : NULL,
					   reply_snap_len, u);
				k_mutex_lock(&s_rel_lock, K_FOREVER);
				continue;
			}
			if (now >= p->deadline_ms) {
				if (p->round + 1 >= CONFIG_NODE_MGR_NN_PROTO_RELIABLE_ROUNDS) {
					NN_LOG_WRN("rel tid=%u gave up (rounds=%u, last to=%u ms)",
						   p->tid, p->round + 1, p->timeout_ms);
					nn_proto_client_reliable_cb_t cb = p->cb;
					void *u = p->user;
					uint32_t tid = p->tid;
					p->active = false;
					k_mutex_unlock(&s_rel_lock);
					if (cb) cb(NN_PROTO_RELIABLE_TIMED_OUT, tid, 0,
						   NULL, 0, u);
					k_mutex_lock(&s_rel_lock, K_FOREVER);
					continue;
				}
				p->round++;
				p->timeout_ms *= 2;
				if (p->timeout_ms > CONFIG_NODE_MGR_NN_PROTO_RELIABLE_MAX_TIMEOUT_MS) {
					p->timeout_ms = CONFIG_NODE_MGR_NN_PROTO_RELIABLE_MAX_TIMEOUT_MS;
				}
				p->deadline_ms = now + p->timeout_ms;
				rel_burst_fire(p);
			}
			if (p->deadline_ms < earliest) earliest = p->deadline_ms;
		}
		k_mutex_unlock(&s_rel_lock);

		int wait_ms = (int)(earliest - nn_osal_uptime_ms());
		if (wait_ms < 5)    wait_ms = 5;
		if (wait_ms > 1000) wait_ms = 1000;
		(void)k_sem_take(&s_rel_wake, K_MSEC(wait_ms));
	}
}

K_THREAD_STACK_DEFINE(s_rel_stack, CONFIG_NODE_MGR_NN_PROTO_RELIABLE_STACK_SIZE);
static struct k_thread s_rel_thread;
static bool            s_rel_thread_started;

static void rel_worker_ensure_started(void)
{
	if (s_rel_thread_started) return;
	k_thread_create(&s_rel_thread, s_rel_stack,
			K_THREAD_STACK_SIZEOF(s_rel_stack),
			rel_worker_fn, NULL, NULL, NULL,
			K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_name_set(&s_rel_thread, "nn_proto_rel");
	s_rel_thread_started = true;
}

static uint32_t s_rel_tid_seq;

static int rel_enqueue(uint16_t frame_type, uint16_t ack_type,
		       const uint8_t peer[16],
		       uint16_t request_cmd, uint16_t ack_cmd,
		       uint32_t caller_tid,
		       const uint8_t *body, size_t body_len,
		       nn_proto_client_reliable_cb_t cb, void *user,
		       bool nosign)
{
	if (!peer)            return -EINVAL;
	if (body_len > REL_BODY_MAX) return -EMSGSIZE;
	if (!S.identity_ready) return NN_PROTO_RELIABLE_NO_ROUTE;
	rel_worker_ensure_started();

	k_mutex_lock(&s_rel_lock, K_FOREVER);
	struct rel_slot *p = NULL;
	for (int i = 0; i < CONFIG_NODE_MGR_NN_PROTO_RELIABLE_INFLIGHT; i++) {
		if (!s_rel[i].active) { p = &s_rel[i]; break; }
	}
	if (!p) {
		k_mutex_unlock(&s_rel_lock);
		return NN_PROTO_RELIABLE_NO_SLOT;
	}
	uint32_t tid;
	if (caller_tid != 0) {
		tid = caller_tid;
	} else {
		if (s_rel_tid_seq == 0) s_rel_tid_seq = 1;
		tid = s_rel_tid_seq++;
	}

	memset(p, 0, sizeof(*p));
	p->active      = true;
	p->frame_type  = frame_type;
	p->ack_type    = ack_type;
	p->request_cmd = request_cmd;
	p->ack_cmd     = ack_cmd;
	memcpy(p->peer, peer, 16);
	p->tid         = tid;
	p->nosign      = nosign || cmd_zero_sig(request_cmd);
	if (body_len) memcpy(p->body, body, body_len);
	p->body_len    = (uint16_t)body_len;
	p->round       = 0;
	p->timeout_ms  = rel_initial_timeout_ms();
	int64_t now    = nn_osal_uptime_ms();
	p->first_send_ms = now;
	p->deadline_ms   = now + p->timeout_ms;
	p->cb          = cb;
	p->user        = user;

	rel_burst_fire(p);

	k_mutex_unlock(&s_rel_lock);
	k_sem_give(&s_rel_wake);
	return 0;
}

int nn_proto_client_send_d2d_reliable(const uint8_t peer_addr[16],
				      uint16_t request_cmd,
				      uint16_t ack_cmd,
				      uint32_t tid,
				      const uint8_t *body, size_t body_len,
				      nn_proto_client_reliable_cb_t cb,
				      void *user)
{
	return rel_enqueue(NN_PROTO_TYPE_D2D, NN_PROTO_TYPE_D2D,
			   peer_addr, request_cmd, ack_cmd, tid,
			   body, body_len, cb, user, false);
}

int nn_proto_client_send_d2d_reliable_ns(const uint8_t peer_addr[16],
					 uint16_t request_cmd,
					 uint16_t ack_cmd,
					 uint32_t tid,
					 const uint8_t *body, size_t body_len,
					 nn_proto_client_reliable_cb_t cb,
					 void *user)
{
	/* Group-sealed D2D body: AEAD authenticates, frame sig zeroed
	 * (D2D receivers never verified sigs anyway). */
	return rel_enqueue(NN_PROTO_TYPE_D2D, NN_PROTO_TYPE_D2D,
			   peer_addr, request_cmd, ack_cmd, tid,
			   body, body_len, cb, user, true);
}

int nn_proto_client_send_d2d_ns(const uint8_t peer_addr[16], uint16_t cmd,
				uint32_t tid,
				const uint8_t *body, size_t body_len)
{
	uint8_t inner[128];
	if (6 + body_len > sizeof(inner)) return -EMSGSIZE;
	nn_osal_put_le16(cmd, inner);
	nn_osal_put_le32(tid, inner + 2);
	if (body_len) memcpy(inner + 6, body, body_len);
	return send_frame_opt(NN_PROTO_TYPE_D2D,
			      S.device_id, NN_PROTO_CLIENT_DEVICE_ID_LEN,
			      inner, 6 + body_len, peer_addr, true);
}

bool nn_proto_client_reliable_tid_active(uint32_t tid)
{
	/* Duplicate-request dedup support: the hub re-sends a request with
	 * the SAME tid when it has not seen the reply yet.  If our reply
	 * for that tid is still in a slot (unacked, retransmitting), the
	 * re-send must NOT be re-served — every serve costs a full ECIES +
	 * ECDSA round on this core and a second slot for the same answer. */
	bool active = false;
	k_mutex_lock(&s_rel_lock, K_FOREVER);
	for (int i = 0; i < CONFIG_NODE_MGR_NN_PROTO_RELIABLE_INFLIGHT; i++) {
		if (s_rel[i].active && !s_rel[i].acked && s_rel[i].tid == tid) {
			active = true;
			break;
		}
	}
	k_mutex_unlock(&s_rel_lock);
	return active;
}

int nn_proto_client_send_d2h_reliable(uint16_t request_cmd,
				      uint16_t ack_cmd,
				      uint32_t tid,
				      const uint8_t *body, size_t body_len,
				      nn_proto_client_reliable_cb_t cb,
				      void *user)
{
	if (!S.gateway_known) return NN_PROTO_RELIABLE_NO_ROUTE;
	return rel_enqueue(NN_PROTO_TYPE_D2H, NN_PROTO_TYPE_H2D,
			   S.gateway_addr, request_cmd, ack_cmd, tid,
			   body, body_len, cb, user, false);
}

int nn_proto_client_send_d2h_reliable_ns(uint16_t request_cmd,
					 uint16_t ack_cmd,
					 uint32_t tid,
					 const uint8_t *body, size_t body_len,
					 nn_proto_client_reliable_cb_t cb,
					 void *user)
{
	/* Session-sealed body: frame signature zeroed (AEAD authenticates).
	 * Same reliable retransmit/ACK machinery otherwise. */
	if (!S.gateway_known) return NN_PROTO_RELIABLE_NO_ROUTE;
	return rel_enqueue(NN_PROTO_TYPE_D2H, NN_PROTO_TYPE_H2D,
			   S.gateway_addr, request_cmd, ack_cmd, tid,
			   body, body_len, cb, user, true);
}

static void dispatch_d2d_frame(const struct nn_proto_view *view,
			       const uint8_t src_addr[16])
{
	if (view->payload_size < 6) return;
	uint16_t cmd  = nn_osal_get_le16(view->payload);
	uint32_t tid  = nn_osal_get_le32(view->payload + 2);
	const uint8_t *body = view->payload + 6;
	size_t body_len = view->payload_size - 6;

	/* First chance: does this match a pending reliable send's ack? */
	if (rel_try_ack(NN_PROTO_TYPE_D2D, cmd, tid, body, body_len)) {
		/* Slot retired; fall through so any registered handler also
		 * sees it (some callers want to observe ACKs explicitly). */
	}

	for (int i = 0; i < (int)ARRAY_SIZE(s_d2d_handlers); i++) {
		if (s_d2d_handlers[i].in_use && s_d2d_handlers[i].cmd == cmd) {
			s_d2d_handlers[i].fn(view->device_id,
					     view->device_id_size,
					     src_addr, tid, body, body_len,
					     s_d2d_handlers[i].user);
			return;
		}
	}
	NN_LOG_DBG("D2D cmd=0x%04x: no handler", cmd);
}

int nn_proto_client_send_d2h_cmd(uint16_t cmd,
				 const uint8_t *body, size_t body_len)
{
	return nn_proto_client_send_d2h_reply(cmd, 0, body, body_len);
}

/* Synchronous request/reply built on top of nn_proto_client_send_d2h_reliable
 * — one underlying retry/RTT/ACK machine for every X2Y pattern in the
 * sensor.  The trampoline below copies the ACK body into the caller's
 * buffer and signals a semaphore.
 *
 * Note: the old single-shot `request_d2h` (no retry) is gone.  Every
 * caller now gets the reliable layer's built-in burst-fire + adaptive
 * backoff for free, which is what they wanted anyway.  `timeout_ms`
 * still governs the *total* wall-clock budget the caller is willing to
 * spend; `max_attempts` and `backoff_*` are kept on the public retry
 * wrapper API for backwards-compatibility but their semantics now
 * collapse onto the reliable layer's NN_PROTO_RELIABLE_ROUNDS schedule.
 */

struct request_sync_ctx {
	struct k_sem  done;
	int           status;
	uint8_t      *out_buf;
	size_t        out_cap;
	size_t        out_len;
};

static void request_sync_cb(nn_proto_reliable_result_t result,
			    uint32_t tid, uint32_t rtt_ms,
			    const uint8_t *reply_body, size_t reply_body_len,
			    void *user)
{
	ARG_UNUSED(tid); ARG_UNUSED(rtt_ms);
	struct request_sync_ctx *c = (struct request_sync_ctx *)user;

	if (result == NN_PROTO_RELIABLE_OK) {
		size_t copy = reply_body_len;
		if (copy > c->out_cap) copy = c->out_cap;
		if (c->out_buf && copy && reply_body) {
			memcpy(c->out_buf, reply_body, copy);
		}
		c->out_len = copy;
		c->status  = 0;
	} else if (result == NN_PROTO_RELIABLE_TIMED_OUT) {
		c->status = -ETIMEDOUT;
	} else if (result == NN_PROTO_RELIABLE_NO_ROUTE) {
		c->status = -ENETUNREACH;
	} else {
		c->status = -EIO;
	}
	k_sem_give(&c->done);
}

int nn_proto_client_request_d2h(uint16_t req_cmd,
				const uint8_t *body, size_t body_len,
				uint16_t expected_reply_cmd,
				uint8_t *reply_body, size_t *reply_body_len,
				uint32_t timeout_ms)
{
	if (!reply_body_len) return -EINVAL;
	struct request_sync_ctx ctx;
	k_sem_init(&ctx.done, 0, 1);
	ctx.status  = -ETIMEDOUT;
	ctx.out_buf = reply_body;
	ctx.out_cap = *reply_body_len;
	ctx.out_len = 0;

	int rv = nn_proto_client_send_d2h_reliable(req_cmd, expected_reply_cmd,
						   0 /* lib-generated tid */,
						   body, body_len,
						   request_sync_cb, &ctx);
	if (rv < 0) {
		*reply_body_len = 0;
		return rv;
	}

	/* Stack-allocated ctx is unsafe if k_sem_take times out before the
	 * reliable worker has delivered its callback — the worker could
	 * later fire cb on a freed stack frame.  Avoid the race by waiting
	 * at least as long as the reliable layer's own deadline schedule
	 * (which is bounded: ROUNDS × geometrically-doubling timeout, capped
	 * at MAX_TIMEOUT_MS per round, plus burst-fire windows).  After
	 * that, the slot is guaranteed to be retired and the cb fired.
	 *
	 * For ROUNDS=4 and MAX_TIMEOUT_MS=4000 the worst case is roughly
	 * MAX_TIMEOUT_MS × ROUNDS = 16 s.  Round up generously to 20 s. */
	uint32_t safe_timeout = timeout_ms;
	if (safe_timeout < 20000) safe_timeout = 20000;
	int sem_rv = k_sem_take(&ctx.done, K_MSEC(safe_timeout));
	if (sem_rv != 0) {
		/* Should not happen — the reliable layer always fires its
		 * callback within its deadline schedule.  Log loudly so we
		 * notice if the invariant is ever violated. */
		printk("request_d2h: BUG sem_take timed out after %u ms — "
		       "reliable layer didn't fire cb\n", safe_timeout);
		*reply_body_len = 0;
		return -ETIMEDOUT;
	}
	*reply_body_len = ctx.out_len;
	return ctx.status;
}

/* Backwards-compat shim: the explicit retry knobs are now subsumed by
 * the reliable layer's internal burst+backoff schedule, so we ignore
 * `max_attempts`, `backoff_ms_base`, `backoff_ms_max` and pass the
 * caller's `timeout_ms` straight through.  Callers that need a longer
 * wall-clock budget should raise `timeout_ms`. */
int nn_proto_client_request_d2h_retry(uint16_t req_cmd,
				      const uint8_t *body, size_t body_len,
				      uint16_t expected_reply_cmd,
				      uint8_t *reply_body,
				      size_t  *reply_body_len,
				      uint32_t timeout_ms,
				      uint8_t  max_attempts,
				      uint32_t backoff_ms_base,
				      uint32_t backoff_ms_max)
{
	ARG_UNUSED(max_attempts);
	ARG_UNUSED(backoff_ms_base);
	ARG_UNUSED(backoff_ms_max);
	return nn_proto_client_request_d2h(req_cmd, body, body_len,
					   expected_reply_cmd,
					   reply_body, reply_body_len,
					   timeout_ms);
}

/* Called from rx_thread when an H2D arrives.  Returns true if the
 * frame was consumed (matched a pending tid or a registered cmd
 * handler); false to fall through to the legacy on_h2d callback. */
static bool dispatch_h2d_inner(const uint8_t *payload, size_t len)
{
	if (len < 6) return false;
	uint16_t cmd = nn_osal_get_le16(payload);
	uint32_t tid = nn_osal_get_le32(payload + 2);
	const uint8_t *body = payload + 6;
	size_t body_len = len - 6;

	/* Reliable-send ACK match — H2D frames may be acks for an
	 * in-flight nn_proto_client_send_d2h_reliable() slot, including
	 * the synchronous request_d2h paths now layered on top of it.
	 * Body is captured for payload-carrying ACKs (OTA_BLOCK / TIME_REPLY). */
	if (rel_try_ack(NN_PROTO_TYPE_H2D, cmd, tid, body, body_len)) {
		return true;
	}

	for (int i = 0; i < (int)ARRAY_SIZE(s_h2d_handlers); i++) {
		if (s_h2d_handlers[i].in_use &&
		    s_h2d_handlers[i].cmd == cmd) {
			s_h2d_handlers[i].fn(tid, body, body_len,
					     s_h2d_handlers[i].user);
			return true;
		}
	}
	/* An ack nobody is waiting for: its slot already gave up (the ack
	 * came LATE) or it was a duplicate for a slot already acked.  Said
	 * out loud because otherwise "arrived late" and "never arrived"
	 * look identical -- both end in "rel tid=N gave up". */
	if (cmd == NN_PROTO_CMD_AUTO_EVENT_ACK ||
	    cmd == NN_PROTO_CMD_FIELD_REPLY_ACK) {
		NN_LOG_INF("unmatched ack cmd=0x%04x tid=%u (late or duplicate)",
			   cmd, tid);
		return true;
	}
	return false;
}

int nn_proto_client_send_d2g(uint16_t cmd,
			     const uint8_t *args, size_t args_len)
{
	if (!S.gateway_known) {
		return -ENETUNREACH;
	}
	uint8_t inner[256];
	if (2 + args_len > sizeof(inner)) {
		return -EMSGSIZE;
	}
	nn_osal_put_le16(cmd, inner);
	if (args_len) {
		memcpy(inner + 2, args, args_len);
	}
	return send_signed_frame(NN_PROTO_TYPE_D2G,
				 S.device_id, NN_PROTO_CLIENT_DEVICE_ID_LEN,
				 inner, 2 + args_len, S.gateway_addr);
}

/* ── HELLO handling ──────────────────────────────────────────────────── */

/* GATEWAY_HELLO inner format (matches docs/protocol/nn_proto.md):
 *   16 B gateway mesh-local IPv6
 *    2 B advertise interval (seconds, LE)
 *    1 B hub-online flag
 */
/* Mesh path cost to a gateway by RLOC16 (gw_cost.c on OpenThread builds). */
__attribute__((weak)) uint8_t nn_proto_client_gw_cost(uint16_t rloc16)
{
	(void)rloc16;
	return NN_GW_COST_UNKNOWN;
}

static void handle_gateway_hello(const uint8_t *args, size_t args_len,
				 const uint8_t src_addr[16])
{
	/* Decision core lives in gw_policy.c (pure, host-tested); this
	 * wrapper only adapts the S.gateway_* fields and logs verdicts.
	 * Policy history: added when the SECOND gateway went live and
	 * last-write-wins made every device flip-flop between the two on
	 * each HELLO — measured ~60/40 D2H split and 11 s field-read
	 * retry outliers, 2026-08-28.  A stale CURRENT gateway with no
	 * rival keeps routing best-effort: HELLO loss is not proof the
	 * data path is dead. */
	/* Full policy state lives across HELLOs (nearest-gateway streak and
	 * dwell); the S.gateway_* fields mirror what the send path needs. */
	static nn_gw_state_t gw;
	gw.known        = S.gateway_known;
	gw.last_seen_ms = S.gateway_last_seen_ms;
	gw.interval_s   = S.gateway_interval_s;
	gw.hub_online   = S.gateway_hub_online;
	memcpy(gw.addr, S.gateway_addr, 16);

	uint8_t cost = nn_proto_client_gw_cost(nn_gw_hello_rloc16(args, args_len));
	nn_gw_verdict_t v = nn_gw_policy_hello_cost(&gw, args, args_len, src_addr,
						    nn_osal_uptime_ms(), cost);
	if (v == NN_GW_MALFORMED || v == NN_GW_KEEP_CURRENT) {
		return;
	}

	if (v == NN_GW_SWITCH_STALE || v == NN_GW_SWITCH_HUB ||
	    v == NN_GW_SWITCH_NEARER || v == NN_GW_ADOPT_FIRST) {
		char ip[NN_OSAL_INET6_ADDRSTRLEN];
		nn_osal_inet_ntop6(gw.addr, ip, sizeof(ip));
		if (v == NN_GW_ADOPT_FIRST) {
			NN_LOG_INF("gateway learned from HELLO: %s", ip);
		} else {
			NN_LOG_INF("gateway switch -> %s (%s, path cost %u)", ip,
				   v == NN_GW_SWITCH_HUB ? "current lost hub" :
				   v == NN_GW_SWITCH_NEARER ? "nearer" : "current stale",
				   gw.cost);
		}
	}

	memcpy(S.gateway_addr, gw.addr, 16);
	S.gateway_last_seen_ms = gw.last_seen_ms;
	S.gateway_interval_s   = gw.interval_s;
	S.gateway_hub_online   = gw.hub_online;
	S.gateway_known        = true;
}

/* ── RX thread ───────────────────────────────────────────────────────── */

static uint32_t s_raw_probe_rx[2];   /* [0] <= 40 B (one frame), [1] larger */

void nn_proto_client_raw_probe_counts(uint32_t *small, uint32_t *large)
{
	*small = s_raw_probe_rx[0];
	*large = s_raw_probe_rx[1];
}

static void rx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	uint8_t buf[RX_BUF_SIZE];

	while (true) {
		nn_osal_sockaddr_in6_t src;
		int n = nn_osal_recvfrom(S.fd, buf, sizeof(buf), 0, &src);
		if (n < 0) {
			NN_LOG_WRN("recvfrom: %d", n);
			nn_osal_sleep_ms(100);
			continue;
		}

		/* Raw delivery probe (gateway diagnostics): "NNPRB" + seq +
		 * padding, sent without the nn_proto envelope so it can be made
		 * small enough for ONE 802.15.4 frame.  Counted silently by size
		 * class (see nn_proto_client_raw_probe_counts). */
		if (n >= 5 && memcmp(buf, "NNPRB", 5) == 0) {
			s_raw_probe_rx[n <= 40 ? 0 : 1]++;
			continue;
		}

		struct nn_proto_view view;
		size_t consumed = 0;
		int rv = nn_proto_parse(buf, (size_t)n, &view, &consumed);
		if (rv != 0 || consumed != (size_t)n) {
			NN_LOG_WRN("parse rv=%d (frame_len=%d)", rv, n);
			continue;
		}

		switch (view.type) {
		case NN_PROTO_TYPE_G2D: {
			if (view.payload_size < 2) break;
			uint16_t cmd = nn_osal_get_le16(view.payload);
			const uint8_t *args = view.payload + 2;
			size_t args_len = view.payload_size - 2;
			if (cmd == NN_PROTO_CMD_GATEWAY_HELLO) {
				handle_gateway_hello(args, args_len, src.addr);
			}
			if (S.cfg.on_g2d) {
				S.cfg.on_g2d(cmd, args, args_len,
					     S.cfg.on_g2d_user);
			}
			break;
		}
		case NN_PROTO_TYPE_H2D: {
			/* New tid-keyed dispatch wins; if nothing claimed it,
			 * fall through to the legacy whole-payload callback so
			 * existing field_relay consumers keep working. */
			if (!dispatch_h2d_inner(view.payload, view.payload_size)
			    && S.cfg.on_h2d) {
				S.cfg.on_h2d(view.payload, view.payload_size,
					     S.cfg.on_h2d_user);
			}
			break;
		}
		case NN_PROTO_TYPE_D2D: {
			/* Intra-mesh peer-to-peer; receivers don't verify the
			 * signature (Thread network-key authenticates at L2). */
			dispatch_d2d_frame(&view, src.addr);
			break;
		}
		default:
			NN_LOG_WRN("dropped unexpected type 0x%04x", view.type);
			break;
		}
	}
}

/* ── public init ─────────────────────────────────────────────────────── */

int nn_proto_client_init(const struct nn_proto_client_config *cfg)
{
	memset(&S, 0, sizeof(S));
	if (cfg) S.cfg = *cfg;
	S.fd = -1;

	int rv = identity_init();
	if (rv) return rv;

	S.fd = nn_osal_socket(NN_OSAL_AF_INET6, NN_OSAL_SOCK_DGRAM,
			      NN_OSAL_IPPROTO_UDP);
	if (S.fd < 0) {
		NN_LOG_ERR("socket: %d", S.fd);
		return S.fd;
	}

	/* Bind UDP_PORT on in6addr_any.  The addr field zero-init is
	 * equivalent to in6addr_any in the OSAL representation. */
	nn_osal_sockaddr_in6_t sa = { .port = UDP_PORT };
	int br = nn_osal_bind(S.fd, &sa);
	if (br < 0) {
		NN_LOG_ERR("bind: %d", br);
		nn_osal_close(S.fd);
		S.fd = -1;
		return br;
	}

	k_thread_create(&s_rx_thread, s_rx_stack,
			K_THREAD_STACK_SIZEOF(s_rx_stack),
			rx_thread, NULL, NULL, NULL,
			WORKER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&s_rx_thread, "nn_proto_rx");

	NN_LOG_INF("nn_proto_client listening on UDP/%u", UDP_PORT);
	return 0;
}

const uint8_t *nn_proto_client_get_device_id(void)
{
	return S.identity_ready ? S.device_id : NULL;
}

const uint8_t *nn_proto_client_get_pubkey(void)
{
	return S.identity_ready ? S.pub : NULL;
}

bool nn_proto_client_gateway_known(void)
{
	return S.gateway_known;
}
