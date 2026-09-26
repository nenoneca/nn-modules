/* SPDX-License-Identifier: Apache-2.0 */

/*
 * HDLC byte-oriented framing for the Spinel link.
 *
 * Frame format:
 *   [0x7E] <payload ...> [FCS-lo] [FCS-hi] [0x7E]
 *
 * Bytes 0x7E (flag) and 0x7D (escape) in the payload and FCS are
 * transmitted as two bytes: 0x7D followed by (original ^ 0x20).
 *
 * FCS is CRC-16/X-25 (polynomial 0x1021, init 0xFFFF, reflected in/out,
 * final XOR 0xFFFF) computed over the raw (pre-escape) payload.
 *
 * The encoder takes a payload + length and writes the fully escaped
 * on-wire bytes to a caller-supplied buffer.
 *
 * The decoder is byte-at-a-time: feed input bytes to hdlc_decode_byte()
 * and it calls the caller's frame handler once a complete, CRC-valid
 * frame has been assembled.
 */

#ifndef NCP_HDLC_H_
#define NCP_HDLC_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define HDLC_FLAG   0x7E
#define HDLC_ESCAPE 0x7D

/* FCS-16/X-25 initial and good-frame residual values */
#define HDLC_FCS_INIT 0xFFFF
#define HDLC_FCS_GOOD 0xF0B8

/*
 * Encode a raw payload as an on-wire HDLC frame (flag-payload-fcs-flag
 * with escapes).  Returns number of bytes written to out_buf, or -1 if
 * out_buf is too small.  Worst case: 2 * (payload_len + 2) + 2 bytes.
 */
int hdlc_encode(const uint8_t *payload, size_t payload_len,
		uint8_t *out_buf, size_t out_cap);

/*
 * Decoder state.  Zero-initialize before first use.
 */
struct hdlc_decoder {
	uint8_t buf[2048];
	size_t  len;
	uint16_t fcs;
	bool in_escape;
	bool in_sync;
	/* Frame callback: invoked with payload (without FCS) on valid frame. */
	void (*on_frame)(const uint8_t *payload, size_t len, void *user);
	void  *user;
};

void hdlc_decoder_init(struct hdlc_decoder *d,
		       void (*on_frame)(const uint8_t *, size_t, void *),
		       void *user);

/* Feed a single byte; triggers on_frame on successful frame end. */
void hdlc_decode_byte(struct hdlc_decoder *d, uint8_t b);

/* Feed a contiguous run of bytes. */
void hdlc_decode(struct hdlc_decoder *d, const uint8_t *data, size_t len);

#endif /* NCP_HDLC_H_ */
