/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_proto — wire format for hub ↔ gateway ↔ device
 *
 * See docs/protocol/nn_proto.md for the full spec.
 *
 * Frame layout (little-endian throughout):
 *
 *   | magic[2]=0x4E 0x4E | type[2] | pkt_size[4] | device_id_size[2] |
 *   | device_id[N] | payload[M] | sig[64] |
 *
 *   pkt_size = bytes from device_id_size through end of sig
 *   sig      = ECDSA P-256 (secp256r1) raw R || S over SHA-256 of
 *              [magic .. payload].  64 bytes total.  Sign side uses
 *              RFC 6979 deterministic-k derivation (PSA_ALG_DETERMINISTIC_ECDSA)
 *              so the embedded path doesn't need an RNG per signature.
 */

#ifndef NN_PROTO_H_
#define NN_PROTO_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NN_PROTO_MAGIC0          0x4Eu  /* 'N' */
#define NN_PROTO_MAGIC1          0x4Eu  /* 'N' */
#define NN_PROTO_HEADER_FIXED    10u    /* magic+type+pkt_size+device_id_size */
#define NN_PROTO_SIG_LEN         64u    /* ECDSA P-256 raw R||S */
#define NN_PROTO_OVERHEAD        (NN_PROTO_HEADER_FIXED + NN_PROTO_SIG_LEN)

/* Frame types (uint16 LE on the wire). */
enum nn_proto_type {
	NN_PROTO_TYPE_D2H = 0x0000,
	NN_PROTO_TYPE_H2D = 0x0001,
	NN_PROTO_TYPE_D2G = 0x0002,
	NN_PROTO_TYPE_G2D = 0x0003,
	/* Device-to-device, intra-mesh.  Sent directly to a peer sensor's
	 * mesh-local address (no hub or gateway in path).  Receivers
	 * accept without verifying the signature — Thread network-key
	 * authentication at L2 covers trust.  Used today by auto_engine
	 * to cascade rule triggers between sensors; replaces the legacy
	 * CoAP /auto path. */
	NN_PROTO_TYPE_D2D = 0x0004,
	/* Hub-to-gateway command, handled by the gateway itself (not
	 * forwarded).  device_id = the gateway id (8 B); signed by the hub,
	 * verified against the hub public key from provisioning.  Inner:
	 * [cmd:2 LE][tid:4 LE][u64 LE epoch ms][body] -- the timestamp and
	 * tid make a captured frame useless for replay.  Replies are D2G
	 * with inner [cmd:2 LE][tid:4 LE][body]. */
	NN_PROTO_TYPE_H2G = 0x0005,
};

/* Inner command codes for D2G/G2D payloads (uint16 LE at payload[0..1]). */
enum nn_proto_cmd {
	NN_PROTO_CMD_RESERVED            = 0x0000,
	NN_PROTO_CMD_HUB_STATUS_QUERY    = 0x0001, /* D2G, no args */
	NN_PROTO_CMD_HUB_STATUS_ANNOUNCE = 0x0002, /* G2D, 1B online flag */
	NN_PROTO_CMD_GATEWAY_HELLO       = 0x0003, /* G2D mcast, 16B addr + 2B interval + 1B online [+ 2B RLOC16 LE] */
	NN_PROTO_CMD_GATEWAY_THREAD_STATE = 0x0004, /* D2G, 1B role + 2B rloc16 LE + 16B mleid */
	NN_PROTO_CMD_DEVICE_HEARTBEAT     = 0x0005, /* D2H fire-and-forget — body: u32 LE uptime_ms.
	                                              * Sensor sends every CONFIG_NODE_MGR_HEARTBEAT_PERIOD_MS
	                                              * so the hub's last_seen stays fresh independent of
	                                              * log/auto-event traffic.  Hub `touch_device`s on every
	                                              * D2H so there's no special handler — but cmd is
	                                              * reserved so hub-side filters / dashboards can
	                                              * recognize it. */
	NN_PROTO_CMD_DEVICE_THREAD_STATE  = 0x0006, /* D2G, 2B did_size LE + did + 16B device ml_eid.
	                                              * Gateway emits one per cached routing-table entry
	                                              * alongside GATEWAY_THREAD_STATE so the hub
	                                              * self-heals each device's ml_eid after sensor
	                                              * reboot (random IID isn't persisted across reboot
	                                              * on the current NVS backend). */

	/* 0x0010..0x001F — hub↔device R/W relay (Phase 6).
	 * Inner payload: [cmd:2 LE | tid:4 LE | ECIES envelope JSON] */
	NN_PROTO_CMD_FIELD_OP        = 0x0010, /* H2D — get/set request, opaque envelope */
	NN_PROTO_CMD_FIELD_REPLY     = 0x0011, /* D2H — paired reply, same tid */
	NN_PROTO_CMD_FIELD_REPLY_ACK = 0x0012, /* H2D — hub ACKs receipt of FIELD_REPLY
	                                        *       so sensor's reliable layer can
	                                        *       retire its retry slot.  Same tid
	                                        *       as the FIELD_REPLY.  Body empty. */

	/* 0x0020..0x002F — sensor↔hub control plane.  Same envelope shape
	 * as FIELD_OP/FIELD_REPLY: each inner payload is
	 *   [cmd:2 LE | tid:4 LE | body ...]
	 * where `body` is cmd-specific (mostly small JSON; OTA_BLOCK is raw
	 * bytes).  D2H carries device-originated requests, H2D carries the
	 * matching hub reply, identified by tid.  Replaces the legacy
	 * CoAP-over-Thread + NAT64 path that lived in coap_*.c + nat64.c. */
	NN_PROTO_CMD_TIME_QUERY     = 0x0020, /* D2H — body empty                       */
	NN_PROTO_CMD_TIME_REPLY     = 0x0021, /* H2D — body: u64 LE epoch ms            */
	NN_PROTO_CMD_LOG_LINE       = 0x0022, /* D2H — body: utf-8 line (no reply)      */
	NN_PROTO_CMD_AUTO_EVENT     = 0x0023, /* D2H — body: JSON event (no reply)      */
	NN_PROTO_CMD_INFO_QUERY     = 0x0024, /* H2D — body empty                       */
	NN_PROTO_CMD_INFO_REPLY     = 0x0025, /* D2H — body: JSON info doc              */
	NN_PROTO_CMD_AUTO_PUSH      = 0x0026, /* H2D — body: ECIES envelope (rule json) */
	NN_PROTO_CMD_AUTO_ACK       = 0x0027, /* D2H — body: 1B status                  */
	NN_PROTO_CMD_COMMISSION_ADD = 0x0028, /* H2D — body: ECIES envelope             */
	NN_PROTO_CMD_COMMISSION_ACK = 0x0029, /* D2H — body: 1B status                  */
	NN_PROTO_CMD_OTA_CHECK      = 0x002a, /* D2H — body: JSON {type,version}        */
	NN_PROTO_CMD_OTA_MANIFEST   = 0x002b, /* H2D — body: JSON {version,size,sha256,slot}
	                                       *             or {update:false} */
	NN_PROTO_CMD_OTA_BLOCK_REQ  = 0x002c, /* D2H — body: u32 LE block_num + u16 LE size */
	NN_PROTO_CMD_OTA_BLOCK      = 0x002d, /* H2D — body: u32 LE block_num + raw bytes  */
	NN_PROTO_CMD_OTA_HINT       = 0x002e, /* H2D — body empty; trigger check+download+apply */
	NN_PROTO_CMD_OTA_HINT_ACK   = 0x002f, /* D2H — body: 1B status (0=accepted)         */
	/* 0x0030/0x0031 are the D2D AUTO_NOTIFY pair below. */
	NN_PROTO_CMD_OTA_CHUNKSUMS_REQ = 0x0032, /* D2H — u32 LE first_block + u16 LE count + opt u8 target (0=image,1=patch) */
	NN_PROTO_CMD_OTA_CHUNKSUMS     = 0x0033, /* H2D — u32 LE first_block + n×8B trunc-sha256 */
	NN_PROTO_CMD_OTA_READY         = 0x0034, /* D2H — JSON {"version","sha256"}: armed, awaiting apply */
	NN_PROTO_CMD_OTA_APPLY         = 0x0035, /* H2D — body empty; request_upgrade + reboot.
	                                           * Device replies OTA_HINT_ACK (status) with the
	                                           * same tid before rebooting. */
	NN_PROTO_CMD_OTA_PATCH_REQ     = 0x0036, /* D2H — u32 LE offset + u16 LE size (detools patch) */
	NN_PROTO_CMD_OTA_PATCH         = 0x0037, /* H2D — u32 LE offset + raw patch bytes */
	NN_PROTO_CMD_SESS_INIT         = 0x0038, /* H2D — 8B hub session salt (Phase 2) */
	NN_PROTO_CMD_SESS_PROBE        = 0x0039, /* D2H — nn_session sealed probe (verify) */
	NN_PROTO_CMD_FIELD_OP_S        = 0x003a, /* H2D — session-sealed field op (Phase 3).
	                                          * body = nn_session record; AAD = cmd+tid LE.
	                                          * Frame sig is ZERO — AEAD tag authenticates. */
	NN_PROTO_CMD_FIELD_REPLY_S     = 0x003b, /* D2H — session-sealed field reply (zero sig) */
	NN_PROTO_CMD_SESS_HELLO        = 0x003c, /* D2H — 8B device session salt.  SMALL frame
	                                          * carrier for the salt (the 537B INFO_REPLY
	                                          * dies to 6LoWPAN fragment loss on weak links).
	                                          * Sent unsigned+retried until SESS_INIT lands. */
	NN_PROTO_CMD_SESS_PROBE_ACK    = 0x003d, /* H2D — empty; hub acks a verified SESS_PROBE */
	NN_PROTO_CMD_SESS_GROUP_KEY    = 0x003e, /* H2D — session-SEALED [epoch:4 LE][key:32] */
	NN_PROTO_CMD_AUTO_NOTIFY_S     = 0x003f, /* D2D — group-keyed AUTO_NOTIFY (zero sig) */
	NN_PROTO_CMD_AUTO_EVENT_ACK    = 0x0040, /* H2D — empty; acks a reliable AUTO_EVENT */
	NN_PROTO_CMD_REBOOT            = 0x0041, /* H2D — body empty; device ACKs then warm-
	                                          * reboots ~500 ms later (ack must leave the
	                                          * radio first).  Same trust surface as
	                                          * OTA_APPLY, which already reboots devices. */
	NN_PROTO_CMD_REBOOT_ACK        = 0x0042, /* D2H — 1B status (0=rebooting) */
	NN_PROTO_CMD_CLEAR_USER_DATA   = 0x0043, /* H2D — session-SEALED [ver:1=01][op:1=01]:
	                                          * arm clear-user-data-on-next-boot.  Only
	                                          * effect is one NVS flag, so retries are
	                                          * SAFE (idempotent) — unlike REBOOT. */
	NN_PROTO_CMD_CLEAR_USER_DATA_ACK = 0x0044, /* D2H — session-SEALED [status:1] */
	NN_PROTO_CMD_RADIO_STATS     = 0x0045, /* D2H — unsigned, tid 0, no reply:
	                                        *       cumulative radio/mesh counters,
	                                        *       node_mgr radio_stats.c (v1 59 B) */

	NN_PROTO_CMD_CHANNEL_SCAN_REQ   = 0x0046, /* H2D — u16 LE ms per channel: the sensor
	                                           *       energy-scans channels 11..26 */
	NN_PROTO_CMD_CHANNEL_SCAN_REPLY = 0x0047, /* D2H — same tid, unacked (the hub
	                                           *       re-asks): [status:i8][channel:u8]
	                                           *       [16 x i8 max dBm, 127 = none] */

	/* 0x0050..0x005F — hub<->gateway control (H2G request, D2G reply;
	 * reply = request + 1, same tid, body starts with status:i8). */
	NN_PROTO_CMD_GW_CHANNEL_SCAN        = 0x0050, /* H2G — u16 LE ms per channel */
	NN_PROTO_CMD_GW_CHANNEL_SCAN_RESULT = 0x0051, /* D2G — [status][channel][16 x i8 dBm] */
	NN_PROTO_CMD_GW_CHANNEL_SET         = 0x0052, /* H2G — u8 channel + u16 LE delay s:
	                                               *       MGMT_PENDING_SET, whole mesh moves
	                                               *       when the delay timer expires */
	NN_PROTO_CMD_GW_CHANNEL_SET_RESULT  = 0x0053, /* D2G — [status] */
	NN_PROTO_CMD_GW_DATASET_GET         = 0x0054, /* H2G — body empty */
	NN_PROTO_CMD_GW_DATASET             = 0x0055, /* D2G — [status][active dataset TLVs] */
	NN_PROTO_CMD_GW_H2D_UNDELIVERABLE   = 0x0056, /* D2G — [reason:i8][did_size:u16 LE][did]
	                                               *       [cmd:u16][tid:u32] of an H2D frame the
	                                               *       gateway could not forward; the hub
	                                               *       re-sends it via another gateway */
	NN_PROTO_CMD_GW_ROUTE_SET           = 0x0058, /* H2G — [did_size:u16 LE][did][16 B mesh addr]:
	                                               *       the hub teaches a gateway where a device
	                                               *       is (it learns only from D2H it carried) */
	NN_PROTO_CMD_GW_ROUTE_SET_RESULT    = 0x0059, /* D2G — [status] */

	/* 0x0030..0x003F — peer-to-peer (D2D) intra-mesh. */
	NN_PROTO_CMD_AUTO_NOTIFY     = 0x0030, /* D2D — body: JSON {auto_id,tid,field,value}.
	                                        *       Sender fires 3× per round at 30 ms gap,
	                                        *       then waits for AUTO_NOTIFY_ACK with
	                                        *       adaptive timeout (last RTT × 2, doubling
	                                        *       each round, capped at MAX_ROUNDS).
	                                        *       Receiver dedups by (sender_id, tid). */
	NN_PROTO_CMD_AUTO_NOTIFY_ACK = 0x0031, /* D2D — body: empty.  Receiver acks each
	                                        *       AUTO_NOTIFY with the same tid so the
	                                        *       sender can retire its slot. */

	/* 0x8000+ reserved for application-defined extensions. */
};

/*
 * Parsed view of an incoming frame.  Pointers reference the original
 * buffer — the caller must keep the buffer alive while the view is in
 * use.  The signature is NOT verified by the parser; call
 * nn_proto_verify_sig() separately.
 */
struct nn_proto_view {
	uint16_t        type;             /* enum nn_proto_type */
	const uint8_t  *device_id;        /* may be NULL if device_id_size==0 */
	uint16_t        device_id_size;
	const uint8_t  *payload;
	size_t          payload_size;
	const uint8_t  *sig;              /* always 64 bytes */

	/* The signed range — bytes [signed_from .. signed_from+signed_len)
	 * within the original buffer.  Equals everything except the sig. */
	const uint8_t  *signed_from;
	size_t          signed_len;
};

/*
 * Parse a buffer into a view.  Validates magic, header fixed size, and
 * pkt_size internal consistency (i.e. the buffer is long enough for the
 * declared frame).  Does NOT verify the signature.
 *
 * Returns 0 on success, or a negative errno (-EINVAL on malformed,
 * -ENOSPC if the buffer is too short for the declared pkt_size).
 *
 * On success, *consumed is set to the number of bytes this frame
 * occupies in the buffer (= 8 + pkt_size).  Useful for parsing
 * back-to-back frames out of a TCP stream.
 */
int nn_proto_parse(const uint8_t *buf, size_t len,
		   struct nn_proto_view *view,
		   size_t *consumed);

/*
 * Compute the encoded frame size for a (device_id, payload).  Returns
 * NN_PROTO_HEADER_FIXED + device_id_size + payload_size + NN_PROTO_SIG_LEN.
 */
static inline size_t
nn_proto_frame_size(uint16_t device_id_size, size_t payload_size)
{
	return (size_t)NN_PROTO_HEADER_FIXED + (size_t)device_id_size +
	       payload_size + (size_t)NN_PROTO_SIG_LEN;
}

/*
 * Encode a frame into a caller-provided buffer.  Writes the full frame
 * including the signature.  The caller supplies a signing callback so
 * this library doesn't take a hard dependency on a particular crypto
 * stack.
 *
 *   sign_fn(ctx, msg, msg_len, sig_out)
 *     msg points to the bytes to be signed (header + device_id +
 *     payload).  sig_out points to a 64-byte buffer the callback
 *     must fill.  Return 0 on success, negative on failure.
 *
 * Returns the number of bytes written on success (== nn_proto_frame_size),
 * or a negative errno (-ENOSPC if buf_len too small, propagated sign_fn
 * error otherwise).
 */
typedef int (*nn_proto_sign_fn)(void *ctx,
				const uint8_t *msg, size_t msg_len,
				uint8_t sig_out[64]);

int nn_proto_encode(uint16_t type,
		    const uint8_t *device_id, uint16_t device_id_size,
		    const uint8_t *payload, size_t payload_size,
		    nn_proto_sign_fn sign_fn, void *sign_ctx,
		    uint8_t *buf, size_t buf_len);

/*
 * Verify the signature on a parsed view using a caller-supplied verify
 * callback (same rationale as the signing callback).
 *
 *   verify_fn(ctx, msg, msg_len, sig)
 *     Returns 0 if signature is valid, negative otherwise.
 */
typedef int (*nn_proto_verify_fn)(void *ctx,
				  const uint8_t *msg, size_t msg_len,
				  const uint8_t sig[64]);

int nn_proto_verify_sig(const struct nn_proto_view *view,
			nn_proto_verify_fn verify_fn, void *verify_ctx);

#ifdef __cplusplus
}
#endif

#endif /* NN_PROTO_H_ */
