/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <nn_proto/nn_proto.h>

/* All multi-byte fields on the wire are little-endian.  We avoid
 * <zephyr/sys/byteorder.h> so this lib also compiles for the Linux
 * host-side build. */

static inline uint16_t rd_u16_le(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t rd_u32_le(const uint8_t *p)
{
	return (uint32_t)p[0]        |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static inline void wr_u16_le(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
}

static inline void wr_u32_le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

int nn_proto_parse(const uint8_t *buf, size_t len,
		   struct nn_proto_view *view,
		   size_t *consumed)
{
	if (!buf || !view) {
		return -EINVAL;
	}
	if (len < NN_PROTO_HEADER_FIXED) {
		return -ENOSPC;
	}
	if (buf[0] != NN_PROTO_MAGIC0 || buf[1] != NN_PROTO_MAGIC1) {
		return -EINVAL;
	}

	uint16_t type           = rd_u16_le(buf + 2);
	uint32_t pkt_size       = rd_u32_le(buf + 4);
	uint16_t device_id_size = rd_u16_le(buf + 8);

	/* pkt_size covers device_id_size[2] + device_id[N] + payload + sig[64]. */
	if (pkt_size < (uint32_t)2 + (uint32_t)device_id_size + NN_PROTO_SIG_LEN) {
		return -EINVAL;
	}
	size_t total_frame_len = (size_t)NN_PROTO_HEADER_FIXED - 2 + pkt_size;
	/* (subtract the 2 bytes of device_id_size that pkt_size counts again
	 * when we walk from offset 0; total_frame_len = 8 + pkt_size). */
	total_frame_len = 8u + (size_t)pkt_size;

	if (len < total_frame_len) {
		return -ENOSPC;
	}

	size_t payload_size = (size_t)pkt_size - 2u -
			      (size_t)device_id_size - NN_PROTO_SIG_LEN;

	view->type            = type;
	view->device_id_size  = device_id_size;
	view->device_id       = device_id_size ? (buf + 10) : NULL;
	view->payload         = buf + 10 + device_id_size;
	view->payload_size    = payload_size;
	view->sig             = buf + 10 + device_id_size + payload_size;

	view->signed_from     = buf;
	view->signed_len      = total_frame_len - NN_PROTO_SIG_LEN;

	if (consumed) {
		*consumed = total_frame_len;
	}
	return 0;
}

int nn_proto_encode(uint16_t type,
		    const uint8_t *device_id, uint16_t device_id_size,
		    const uint8_t *payload, size_t payload_size,
		    nn_proto_sign_fn sign_fn, void *sign_ctx,
		    uint8_t *buf, size_t buf_len)
{
	if (!sign_fn || !buf) {
		return -EINVAL;
	}
	if (device_id_size > 0 && !device_id) {
		return -EINVAL;
	}
	if (payload_size > 0 && !payload) {
		return -EINVAL;
	}

	size_t frame_size = nn_proto_frame_size(device_id_size, payload_size);
	if (buf_len < frame_size) {
		return -ENOSPC;
	}

	/* pkt_size = bytes from device_id_size through end of sig. */
	uint32_t pkt_size = 2u + (uint32_t)device_id_size +
			    (uint32_t)payload_size + NN_PROTO_SIG_LEN;

	buf[0] = NN_PROTO_MAGIC0;
	buf[1] = NN_PROTO_MAGIC1;
	wr_u16_le(buf + 2, type);
	wr_u32_le(buf + 4, pkt_size);
	wr_u16_le(buf + 8, device_id_size);
	if (device_id_size) {
		memcpy(buf + 10, device_id, device_id_size);
	}
	if (payload_size) {
		memcpy(buf + 10 + device_id_size, payload, payload_size);
	}

	/* Signature covers everything we just wrote (header + id + payload). */
	uint8_t *sig_out = buf + 10 + device_id_size + payload_size;
	size_t   signed_len = (size_t)(sig_out - buf);

	int rv = sign_fn(sign_ctx, buf, signed_len, sig_out);
	if (rv != 0) {
		return rv;
	}
	return (int)frame_size;
}

int nn_proto_verify_sig(const struct nn_proto_view *view,
			nn_proto_verify_fn verify_fn, void *verify_ctx)
{
	if (!view || !verify_fn) {
		return -EINVAL;
	}
	return verify_fn(verify_ctx, view->signed_from, view->signed_len,
			 view->sig);
}
