/* SPDX-License-Identifier: Apache-2.0 */

#include <fw_common/spinel.h>
#include <fw_common/log.h>

LOG_MODULE_REGISTER(spinel, LOG_LEVEL_INF);

int spinel_pack_uint(uint8_t *buf, size_t cap, uint32_t value)
{
	size_t n = 0;

	do {
		if (n >= cap) {
			return -1;
		}
		uint8_t b = value & 0x7f;
		value >>= 7;
		if (value != 0) {
			b |= 0x80;
		}
		buf[n++] = b;
	} while (value != 0);

	return (int)n;
}

int spinel_unpack_uint(const uint8_t *buf, size_t len, uint32_t *out)
{
	uint32_t value = 0;
	size_t n = 0;
	unsigned shift = 0;

	while (n < len) {
		uint8_t b = buf[n++];
		value |= (uint32_t)(b & 0x7f) << shift;
		if ((b & 0x80) == 0) {
			*out = value;
			return (int)n;
		}
		shift += 7;
		if (shift > 28) {
			return -1; /* overflow */
		}
	}

	return -1; /* truncated */
}

int spinel_build_get(uint8_t *out_buf, size_t cap,
		     uint8_t tid, uint32_t prop)
{
	if (cap < 1) {
		return -1;
	}
	size_t n = 0;
	out_buf[n++] = SPINEL_HEADER_FLAG | (tid & 0x0f);

	int r = spinel_pack_uint(out_buf + n, cap - n, SPINEL_CMD_PROP_VALUE_GET);
	if (r < 0) return -1;
	n += (size_t)r;

	r = spinel_pack_uint(out_buf + n, cap - n, prop);
	if (r < 0) return -1;
	n += (size_t)r;

	return (int)n;
}

int spinel_build_cmd(uint8_t *out_buf, size_t cap,
		     uint8_t tid, uint32_t cmd, uint32_t prop,
		     const uint8_t *payload, size_t payload_len)
{
	if (cap < 1) {
		return -1;
	}
	size_t n = 0;
	out_buf[n++] = SPINEL_HEADER_FLAG | (tid & 0x0f);

	int r = spinel_pack_uint(out_buf + n, cap - n, cmd);
	if (r < 0) return -1;
	n += (size_t)r;

	r = spinel_pack_uint(out_buf + n, cap - n, prop);
	if (r < 0) return -1;
	n += (size_t)r;

	if (payload_len > 0) {
		if (n + payload_len > cap) return -1;
		if (payload) {
			for (size_t i = 0; i < payload_len; i++) {
				out_buf[n + i] = payload[i];
			}
		}
		n += payload_len;
	}

	return (int)n;
}

int spinel_build_set(uint8_t *out_buf, size_t cap,
		     uint8_t tid, uint32_t prop,
		     const uint8_t *payload, size_t payload_len)
{
	return spinel_build_cmd(out_buf, cap, tid,
				SPINEL_CMD_PROP_VALUE_SET, prop,
				payload, payload_len);
}

int spinel_parse(const uint8_t *buf, size_t len, struct spinel_frame *out)
{
	if (len < 2) {
		return -1;
	}

	uint8_t hdr = buf[0];
	if ((hdr & 0xC0) != SPINEL_HEADER_FLAG) {
		return -1;
	}

	out->header = hdr;
	out->iid    = (hdr >> 4) & 0x03;
	out->tid    = hdr & 0x0f;
	out->has_prop = false;
	out->prop   = 0;
	out->value  = NULL;
	out->value_len = 0;

	size_t off = 1;

	int r = spinel_unpack_uint(buf + off, len - off, &out->cmd);
	if (r < 0) return -1;
	off += (size_t)r;

	/* PROP_VALUE_* commands carry a packed property after the cmd. */
	if (out->cmd == SPINEL_CMD_PROP_VALUE_IS ||
	    out->cmd == SPINEL_CMD_PROP_VALUE_GET ||
	    out->cmd == SPINEL_CMD_PROP_VALUE_SET ||
	    out->cmd == SPINEL_CMD_PROP_VALUE_INSERT ||
	    out->cmd == SPINEL_CMD_PROP_VALUE_REMOVE ||
	    out->cmd == SPINEL_CMD_PROP_VALUE_INSERTED ||
	    out->cmd == SPINEL_CMD_PROP_VALUE_REMOVED) {
		r = spinel_unpack_uint(buf + off, len - off, &out->prop);
		if (r < 0) return -1;
		off += (size_t)r;
		out->has_prop = true;
	}

	out->value = (off <= len) ? buf + off : NULL;
	out->value_len = (off <= len) ? len - off : 0;

	return 0;
}

const char *spinel_status_str(uint32_t status)
{
	switch (status) {
	case SPINEL_STATUS_OK:                 return "ok";
	case SPINEL_STATUS_FAILURE:            return "failure";
	case SPINEL_STATUS_UNIMPLEMENTED:      return "unimplemented";
	case SPINEL_STATUS_INVALID_ARGUMENT:   return "invalid-argument";
	case 4:                                return "invalid-state";
	case 5:                                return "invalid-command";
	case 6:                                return "invalid-interface";
	case 7:                                return "internal-error";
	case 8:                                return "security-error";
	case 9:                                return "parse-error";
	case 10:                               return "in-progress";
	case 11:                               return "nomem";
	case 12:                               return "busy";
	case 13:                               return "prop-not-found";
	case 14:                               return "dropped";
	case 15:                               return "empty";
	case 16:                               return "cmd-too-big";
	case 17:                               return "no-ack";
	case 18:                               return "cca-failure";
	case 19:                               return "already";
	case 20:                               return "item-not-found";
	case 21:                               return "invalid-cmd-for-prop";
	case SPINEL_STATUS_RESET_POWER_ON:     return "reset:power-on";
	case SPINEL_STATUS_RESET_UNKNOWN:      return "reset:unknown";
	}
	if (status >= 112 && status <= 127) {
		return "reset";
	}
	return "unknown";
}
