/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_proto_client — device-side UDP transport for the nn_proto wire
 * format.  Replaces the CoAP path in node_mgr.
 *
 *   - Listens on CONFIG_NODE_MGR_NN_PROTO_PORT for inbound G2D + H2D
 *     frames coming from the gateway.
 *   - Caches the gateway's mesh-local IPv6 from incoming
 *     GATEWAY_HELLO multicasts.
 *   - send_d2h() and send_d2g() build + sign + transmit a frame to
 *     that cached gateway address.
 *   - Caller registers handlers for H2D (hub-originated) and G2D
 *     (gateway-originated) frames.
 *
 * Identity: this module owns its own P-256 keypair (NVS-persisted under
 * settings root "nn_proto_dev/p256_priv").  The 8-byte device_id is
 * derived as SHA256(pubkey)[:8] and is what the hub indexes by.
 */

#ifndef NODE_MGR_NN_PROTO_CLIENT_H_
#define NODE_MGR_NN_PROTO_CLIENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NN_PROTO_CLIENT_DEVICE_ID_LEN  8
#define NN_PROTO_CLIENT_PUBKEY_LEN     65

/* Inner-frame handlers (caller sees the parsed view's payload).
 * payload may include the 2-byte cmd prefix for D2G/G2D; for H2D
 * it's the full opaque payload (typically an ECIES v2 ciphertext). */
typedef void (*nn_proto_client_h2d_fn)(const uint8_t *payload, size_t len,
				       void *user);
typedef void (*nn_proto_client_g2d_fn)(uint16_t cmd,
				       const uint8_t *args, size_t args_len,
				       void *user);

struct nn_proto_client_config {
	nn_proto_client_h2d_fn  on_h2d;
	void                   *on_h2d_user;
	nn_proto_client_g2d_fn  on_g2d;
	void                   *on_g2d_user;
};

/* Initialize the client (keys + UDP listener + RX thread). */
int nn_proto_client_init(const struct nn_proto_client_config *cfg);

/* Read accessors (NULL until init runs). */
const uint8_t *nn_proto_client_get_device_id(void);   /* 8 bytes */
const uint8_t *nn_proto_client_get_pubkey(void);      /* 65 bytes uncompressed */

/* True if a GATEWAY_HELLO has been heard recently.  Phase-3 simple
 * staleness check: anything heard since boot counts. */
bool nn_proto_client_gateway_known(void);

/* Build + sign + send a D2H frame.  payload is the opaque body —
 * typically an ECIES v2 ciphertext intended for the hub.  Returns 0
 * on success or negative errno (no cached gateway, send failure, etc.). */
int nn_proto_client_send_d2h(const uint8_t *payload, size_t payload_len);

/* Build + sign + send a D2G frame with the given cmd + optional args. */
int nn_proto_client_send_d2g(uint16_t cmd,
			     const uint8_t *args, size_t args_len);

/* ── D2H request / H2D reply ────────────────────────────────────────
 *
 * Sensor↔hub request/reply helpers built on top of D2H/H2D.  Wire
 * shape (matches the existing FIELD_OP/FIELD_REPLY convention):
 *
 *   D2H payload = [u16 LE cmd | u32 LE tid | body bytes]
 *   H2D payload = [u16 LE cmd | u32 LE tid | body bytes]
 *
 * Reply is matched by tid AND by cmd (caller passes expected reply
 * cmd).  Up to NN_PROTO_CLIENT_MAX_PENDING outstanding requests in
 * flight; older slots are recycled LRU on overflow.
 *
 * Send a D2H request and wait synchronously for the matching H2D
 * reply.  Body bytes are copied into the outgoing frame; reply body
 * (everything after [cmd|tid]) is copied into reply_body up to
 * *reply_body_len.  Returns 0 on success and writes the actual reply
 * size back into *reply_body_len.  Negative errno on failure
 * (-ETIMEDOUT, -ENOMEM, -ENETUNREACH, …).
 */
#define NN_PROTO_CLIENT_MAX_PENDING  4

/* Single-attempt: send once, wait up to timeout_ms for a matching
 * H2D reply.  Caller responsible for any retry policy. */
int nn_proto_client_request_d2h(uint16_t req_cmd,
				const uint8_t *body, size_t body_len,
				uint16_t expected_reply_cmd,
				uint8_t *reply_body, size_t *reply_body_len,
				uint32_t timeout_ms);

/* Backoff-equipped version of request_d2h.  Mirrors the hub's
 * HubProtoRouter.request_h2d retry policy so every X2Y request/reply
 * pattern in the system has the same shape:
 *
 *   for attempt in 1..max_attempts:
 *       single-shot request_d2h(... timeout_ms)
 *       on -ETIMEDOUT / -EIO: sleep backoff_ms_base << (attempt-1)
 *       (sleep capped at backoff_ms_max)
 *       refresh tid and retry
 *
 * Pass max_attempts=1 to disable retry (equivalent to plain
 * request_d2h).  Defaults the public API chooses are tuned for
 * sensor↔hub on a flaky Thread mesh: 5 × 8 s timeout + (1+2+4+8) s
 * backoff = ~55 s wall time worst case. */
int nn_proto_client_request_d2h_retry(uint16_t req_cmd,
				      const uint8_t *body, size_t body_len,
				      uint16_t expected_reply_cmd,
				      uint8_t *reply_body,
				      size_t  *reply_body_len,
				      uint32_t timeout_ms,
				      uint8_t  max_attempts,
				      uint32_t backoff_ms_base,
				      uint32_t backoff_ms_max);

/* Send a fire-and-forget D2H with [cmd|tid=0|body].  No reply
 * tracking; caller doesn't wait.  Use for LOG_LINE / AUTO_EVENT
 * style telemetry. */
int nn_proto_client_send_d2h_cmd(uint16_t cmd,
				 const uint8_t *body, size_t body_len);

/* Monotonic uptime_ms at which the LAST successful D2H frame went
 * out the radio (any type — LOG_LINE, AUTO_EVENT, FIELD_REPLY,
 * HEARTBEAT, OTA_BLOCK_REQ, …).  Returns 0 if no D2H has been sent
 * yet.  Used by the heartbeat module to skip a fire when other
 * traffic has already refreshed the hub's last_seen. */
int64_t nn_proto_client_last_d2h_send_ms(void);

/* Register an H2D dispatcher by cmd code.  When an H2D frame arrives
 * with tid=0 (or no pending request matches), this dispatcher fires
 * if the cmd matches.  Used for hub-initiated requests like
 * INFO_QUERY / AUTO_PUSH / COMMISSION_ADD where the hub asks the
 * sensor to do something.
 *
 * payload is everything after [cmd|tid] in the H2D inner payload.
 * If the dispatcher wants to reply, build [reply_cmd|tid|body] and
 * call nn_proto_client_send_d2h_raw() with the SAME tid (caller-side
 * routine handles tid).
 *
 * Only one dispatcher per cmd; latest registration wins.
 */
typedef void (*nn_proto_client_h2d_cmd_fn)(uint32_t tid,
					   const uint8_t *body, size_t body_len,
					   void *user);

int nn_proto_client_register_h2d_handler(uint16_t cmd,
					 nn_proto_client_h2d_cmd_fn handler,
					 void *user);

/* Build + sign + send a D2H frame whose inner payload is
 *   [cmd:2 LE | tid:4 LE | body...]
 * Caller-controlled tid; for fire-and-forget set tid=0.  Used by
 * h2d-handler reply paths. */
int nn_proto_client_send_d2h_reply(uint16_t cmd, uint32_t tid,
				   const uint8_t *body, size_t body_len);

/* ── D2D peer-to-peer (intra-mesh) ─────────────────────────────────
 *
 * Sensor↔sensor frames sent directly to a peer's mesh-local address.
 * The wire shape mirrors D2H/H2D:
 *   D2D payload = [u16 LE cmd | u32 LE tid | body bytes]
 *
 * Reliability: callers use 3× fire at 30 ms gap (per
 * feedback_auto_engine_coap_retry.md); receivers dedup by
 * (sender_device_id, tid).  No application-level ACK.  Signatures
 * present on the wire but receivers don't verify — Thread network-key
 * authentication at L2 is the trust boundary for intra-mesh traffic.
 */

/* Send a single D2D frame to a peer's 16-byte IPv6 address.  Caller
 * supplies the tid (use a random uint32 + retry with same tid for the
 * dedup contract). */
int nn_proto_client_send_d2d(const uint8_t peer_addr[16],
			     uint16_t cmd, uint32_t tid,
			     const uint8_t *body, size_t body_len);

/* D2D inbound dispatcher.  Receiver sees the sender's device_id, its
 * IPv6 source address (so the handler can reply via
 * nn_proto_client_send_d2d), the inner cmd + tid + body.  Dedup is
 * the receiver's responsibility (use device_id + tid as the key). */
typedef void (*nn_proto_client_d2d_cmd_fn)(const uint8_t *sender_device_id,
					   size_t sender_device_id_len,
					   const uint8_t sender_ipv6[16],
					   uint32_t tid,
					   const uint8_t *body, size_t body_len,
					   void *user);

int nn_proto_client_register_d2d_handler(uint16_t cmd,
					 nn_proto_client_d2d_cmd_fn handler,
					 void *user);

/* ── Reliable mesh send (D2H or D2D) ───────────────────────────────
 *
 * Asynchronous send with bounded retry and adaptive timeout — same
 * shape across all mesh paths (D2H sensor→hub via gateway, D2D
 * peer-to-peer).  The caller hands over a body + an ack_cmd that the
 * peer will respond with; the library takes care of:
 *
 *   - Burst-fire: each round sends `fire_burst` copies back-to-back at
 *     `fire_gap_ms` to absorb 5-30%/hop radio drops.
 *   - Adaptive initial timeout: max(MIN_TIMEOUT_MS, min(MAX_TIMEOUT_MS,
 *                                  last_RTT × 2)).  Until the first
 *     successful ACK, INIT_TIMEOUT_MS is used.
 *   - Exponential backoff: timeout × 2 each round, capped at MAX.
 *   - Bounded retry: gives up after `max_rounds` rounds.
 *   - RTT estimation: updated on every successful ACK.
 *
 * `on_done` (optional) is invoked exactly once per send, on the
 * library's worker thread, with the final outcome.  Set to NULL for
 * fire-and-track (caller doesn't care about outcome but still wants
 * the retry logic to drive the wire).
 *
 * Receivers MUST send back a single frame of the matching ack_cmd
 * with the same tid (and same frame type — D2D ACKs go back via D2D,
 * D2H ACKs come back as H2D).  See node_mgr/auto_engine.c for a
 * worked example.
 */
typedef enum {
	NN_PROTO_RELIABLE_OK        = 0,
	NN_PROTO_RELIABLE_TIMED_OUT = -1,
	NN_PROTO_RELIABLE_NO_SLOT   = -2,
	NN_PROTO_RELIABLE_NO_ROUTE  = -3,
} nn_proto_reliable_result_t;

/* `reply_body` / `reply_body_len` carry the ACK's payload bytes (the
 * inner-frame contents after [cmd|tid]).  For payload-carrying ACKs
 * like OTA_BLOCK / TIME_REPLY / INFO_REPLY they hold the reply data;
 * for fire-and-track patterns like FIELD_REPLY_ACK / AUTO_NOTIFY_ACK
 * they are NULL / 0.  Buffer is valid for the duration of the callback
 * only — copy out if you need to keep it. */
typedef void (*nn_proto_client_reliable_cb_t)(
	nn_proto_reliable_result_t result,
	uint32_t tid,
	uint32_t rtt_ms,
	const uint8_t *reply_body, size_t reply_body_len,
	void *user);

/* Send a D2D request reliably to a peer.  `ack_cmd` is the D2D
 * command code the peer will reply with.  `tid`: pass 0 to use the
 * library-generated sequence; pass a specific value to keep the
 * inbound request's tid when sending a reply (e.g. FIELD_REPLY).
 * Returns 0 if the slot was accepted (the on_done callback will
 * eventually fire), negative on immediate failure. */
/* Zero-sig variants for group-sealed D2D (AEAD carries authenticity). */
int nn_proto_client_send_d2d_reliable_ns(const uint8_t peer_addr[16],
					 uint16_t request_cmd,
					 uint16_t ack_cmd,
					 uint32_t tid,
					 const uint8_t *body, size_t body_len,
					 nn_proto_client_reliable_cb_t cb,
					 void *user);
int nn_proto_client_send_d2d_ns(const uint8_t peer_addr[16], uint16_t cmd,
				uint32_t tid,
				const uint8_t *body, size_t body_len);

int nn_proto_client_send_d2d_reliable(const uint8_t peer_addr[16],
				      uint16_t request_cmd,
				      uint16_t ack_cmd,
				      uint32_t tid,
				      const uint8_t *body, size_t body_len,
				      nn_proto_client_reliable_cb_t cb,
				      void *user);

/* Send a D2H request reliably to the hub (via the cached gateway).
 * `ack_cmd` is the H2D command code the hub will reply with.  `tid`
 * follows the same rule as the D2D variant. */
/* True while an unacked reliable D2H slot exists for *tid* — used by
 * request handlers to dedup hub re-sends instead of re-serving them. */
bool nn_proto_client_reliable_tid_active(uint32_t tid);

/* Reliable D2H whose frame signature is ZERO — for session-sealed
 * bodies (FIELD_REPLY_S): the AEAD tag authenticates, skipping ~1 s of
 * ECDSA per frame on this core. */
/* Unsigned (zero-sig) fire-and-forget D2H — see send_d2h_reply. */
int nn_proto_client_send_d2h_reply_ns(uint16_t cmd, uint32_t tid,
				      const uint8_t *body, size_t body_len);

int nn_proto_client_send_d2h_reliable_ns(uint16_t request_cmd,
					 uint16_t ack_cmd,
					 uint32_t tid,
					 const uint8_t *body, size_t body_len,
					 nn_proto_client_reliable_cb_t cb,
					 void *user);

int nn_proto_client_send_d2h_reliable(uint16_t request_cmd,
				      uint16_t ack_cmd,
				      uint32_t tid,
				      const uint8_t *body, size_t body_len,
				      nn_proto_client_reliable_cb_t cb,
				      void *user);

#ifdef __cplusplus
}
#endif

/* Diagnostics: raw delivery probes received (<=40 B, larger). */
void nn_proto_client_raw_probe_counts(uint32_t *small, uint32_t *large);

#endif /* NODE_MGR_NN_PROTO_CLIENT_H_ */
