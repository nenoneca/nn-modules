/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Minimal Spinel encoder/decoder.
 *
 * Covers what Milestone A needs: framing a Spinel request with a
 * packed-unsigned-int command + property, and parsing an inbound
 * PROP_VALUE_IS response for a known property.
 *
 * Spinel header byte layout:
 *   bit 7..6  = 0b10 (protocol version flag, always set)
 *   bit 5..4  = IID  (interface id; 0 for single-interface NCP)
 *   bit 3..0  = TID  (transaction id; 0 = unsolicited, 1..15 = request)
 *
 * Packed unsigned int:
 *   Little-endian, 7 bits of data per byte, MSB set on all but the
 *   last byte.
 *
 * Full spec: third_party/openthread/src/lib/spinel/spinel.h
 */

#ifndef NCP_SPINEL_H_
#define NCP_SPINEL_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Header flag (top two bits). */
#define SPINEL_HEADER_FLAG 0x80

/* Commands we need on the host side. */
#define SPINEL_CMD_NOOP              0
#define SPINEL_CMD_RESET             1
#define SPINEL_CMD_PROP_VALUE_GET    2
#define SPINEL_CMD_PROP_VALUE_SET    3
#define SPINEL_CMD_PROP_VALUE_INSERT 4
#define SPINEL_CMD_PROP_VALUE_REMOVE 5
#define SPINEL_CMD_PROP_VALUE_IS     6
#define SPINEL_CMD_PROP_VALUE_INSERTED 7
#define SPINEL_CMD_PROP_VALUE_REMOVED  8

/* Properties (core). */
#define SPINEL_PROP_LAST_STATUS      0
#define SPINEL_PROP_PROTOCOL_VERSION 1
#define SPINEL_PROP_NCP_VERSION      2
#define SPINEL_PROP_INTERFACE_TYPE   3
#define SPINEL_PROP_CAPS             5
#define SPINEL_PROP_HWADDR           8

/* PHY / MAC layer. */
#define SPINEL_PROP_PHY_ENABLED      0x20
#define SPINEL_PROP_PHY_CHAN         0x21 /* C */
#define SPINEL_PROP_MAC_15_4_LADDR   0x34 /* EUI-64 */
#define SPINEL_PROP_MAC_15_4_PANID   0x36 /* S (u16) */

/* Thread NET layer. */
#define SPINEL_PROP_NET_SAVED        0x40 /* b (ro) */
#define SPINEL_PROP_NET_IF_UP        0x41 /* b */
#define SPINEL_PROP_NET_STACK_UP     0x42 /* b */
#define SPINEL_PROP_NET_ROLE         0x43 /* C */
#define SPINEL_PROP_NET_NETWORK_NAME 0x44 /* U */
#define SPINEL_PROP_NET_XPANID       0x45 /* D (8 bytes) */
#define SPINEL_PROP_NET_NETWORK_KEY  0x46 /* D (16 bytes) */

/* IPv6 layer. */
#define SPINEL_PROP_IPV6_LL_ADDR       0x60 /* 6 (ro) */
#define SPINEL_PROP_IPV6_ML_ADDR       0x61 /* 6 (ro) */
#define SPINEL_PROP_IPV6_ML_PREFIX     0x62 /* 6C */
#define SPINEL_PROP_IPV6_ADDRESS_TABLE 0x63 /* A(t(6CLL)) */

/* Thread routing / network data. */
#define SPINEL_PROP_THREAD_ON_MESH_NETS              0x5A /* A(t(6CbCbSC)) */
#define SPINEL_PROP_THREAD_OFF_MESH_ROUTES           0x5B /* A(t(6CbCbb)) */
#define SPINEL_PROP_THREAD_ALLOW_LOCAL_NET_DATA_CHANGE 0x5D /* b */

/* Thread extended (>=0x1500, packed-uint encoded on the wire). */
#define SPINEL_PROP_THREAD_RLOC16                    0x1501 /* S (u16) */

/* Data-plane streams. */
#define SPINEL_PROP_STREAM_NET          0x72 /* dD — packet + meta */
#define SPINEL_PROP_STREAM_NET_INSECURE 0x73

/* THREAD_EXT range for UDP forwarding.  Packed-uint encoded on wire. */
#define SPINEL_PROP_THREAD_UDP_FORWARD_STREAM 0x1524 /* dS6S */

/* Net role codes (for NET_ROLE property). */
#define SPINEL_NET_ROLE_DETACHED 0
#define SPINEL_NET_ROLE_CHILD    1
#define SPINEL_NET_ROLE_ROUTER   2
#define SPINEL_NET_ROLE_LEADER   3
#define SPINEL_NET_ROLE_DISABLED 4

/* LAST_STATUS codes we care about (subset of many). */
#define SPINEL_STATUS_OK                       0
#define SPINEL_STATUS_FAILURE                  1
#define SPINEL_STATUS_UNIMPLEMENTED            2
#define SPINEL_STATUS_INVALID_ARGUMENT         3
#define SPINEL_STATUS_RESET_POWER_ON        112
#define SPINEL_STATUS_RESET_UNKNOWN         114

/*
 * Encode a packed unsigned int into buf/buf_end.  Returns number of
 * bytes written, or -1 if out of space.
 */
int spinel_pack_uint(uint8_t *buf, size_t cap, uint32_t value);

/*
 * Decode a packed unsigned int.  Returns number of bytes consumed and
 * stores the value in *out; returns -1 if malformed or truncated.
 */
int spinel_unpack_uint(const uint8_t *buf, size_t len, uint32_t *out);

/*
 * Build a CMD_PROP_VALUE_GET frame (header + cmd + prop) into out_buf.
 * Returns number of bytes or -1 if too small.  Caller then passes the
 * frame into hdlc_encode().
 */
int spinel_build_get(uint8_t *out_buf, size_t cap,
		     uint8_t tid, uint32_t prop);

/*
 * Build a CMD_PROP_VALUE_SET frame.  `payload` is the already-packed
 * property value bytes (e.g. a single uint8 for bool/C, a little-endian
 * uint16 for S, raw bytes for D, or UTF-8 + NUL for U).  Returns total
 * pre-HDLC frame bytes written, or -1.
 */
int spinel_build_set(uint8_t *out_buf, size_t cap,
		     uint8_t tid, uint32_t prop,
		     const uint8_t *payload, size_t payload_len);

/*
 * Same as spinel_build_set but picks the command byte.  Used for
 * SET/INSERT/REMOVE where the payload layout is the same but the
 * semantics differ.
 */
int spinel_build_cmd(uint8_t *out_buf, size_t cap,
		     uint8_t tid, uint32_t cmd, uint32_t prop,
		     const uint8_t *payload, size_t payload_len);

/*
 * Decoded view of an inbound frame.
 */
struct spinel_frame {
	uint8_t  header;
	uint8_t  iid;
	uint8_t  tid;
	uint32_t cmd;
	uint32_t prop;            /* valid when cmd is PROP_VALUE_IS */
	bool     has_prop;
	const uint8_t *value;
	size_t   value_len;
};

/*
 * Parse an inbound Spinel frame (already stripped of HDLC framing +
 * FCS).  Populates *out; returns 0 on success, -1 on malformed input.
 */
int spinel_parse(const uint8_t *buf, size_t len, struct spinel_frame *out);

/*
 * Human-readable name for well-known status codes.  Returns "unknown"
 * for codes we don't recognize.
 */
const char *spinel_status_str(uint32_t status);

#endif /* NCP_SPINEL_H_ */
