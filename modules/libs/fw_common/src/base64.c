/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <fw_common/base64.h>

static const char b64_alphabet[64] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_decode_char(unsigned char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
	if (c >= '0' && c <= '9') return 52 + (c - '0');
	if (c == '+')             return 62;
	if (c == '/')             return 63;
	return -1; /* invalid char (or '=' padding handled by caller) */
}

int fw_base64_encode(uint8_t *dst, size_t dlen, size_t *olen,
		     const uint8_t *src, size_t slen)
{
	if (!olen) return -EINVAL;

	size_t need = ((slen + 2) / 3) * 4;
	if (dst == NULL && dlen == 0) {
		*olen = need + 1; /* include NUL byte for parity with Zephyr */
		return 0;
	}
	if (need + 1 > dlen) {
		*olen = need + 1;
		return -ENOMEM;
	}

	size_t i = 0, j = 0;
	while (i + 3 <= slen) {
		uint32_t v = ((uint32_t)src[i]   << 16) |
			     ((uint32_t)src[i+1] <<  8) |
			      (uint32_t)src[i+2];
		dst[j++] = b64_alphabet[(v >> 18) & 0x3f];
		dst[j++] = b64_alphabet[(v >> 12) & 0x3f];
		dst[j++] = b64_alphabet[(v >>  6) & 0x3f];
		dst[j++] = b64_alphabet[ v        & 0x3f];
		i += 3;
	}
	if (i < slen) {
		uint32_t v = (uint32_t)src[i] << 16;
		size_t left = slen - i;
		if (left == 2) {
			v |= (uint32_t)src[i+1] << 8;
		}
		dst[j++] = b64_alphabet[(v >> 18) & 0x3f];
		dst[j++] = b64_alphabet[(v >> 12) & 0x3f];
		dst[j++] = (left == 2) ? b64_alphabet[(v >> 6) & 0x3f] : '=';
		dst[j++] = '=';
	}
	dst[j] = '\0';
	*olen = j;
	return 0;
}

int fw_base64_decode(uint8_t *dst, size_t dlen, size_t *olen,
		     const uint8_t *src, size_t slen)
{
	if (!olen) return -EINVAL;

	/* Strip optional trailing whitespace and '=' for length calc. */
	size_t end = slen;
	while (end > 0 && (src[end-1] == '\n' || src[end-1] == '\r' ||
			   src[end-1] == ' ' || src[end-1] == '\t' ||
			   src[end-1] == '=')) {
		end--;
	}

	/* Each 4 b64 chars → 3 bytes; trailing 2 chars → 1 byte, 3 → 2. */
	size_t full_groups = end / 4;
	size_t tail = end - full_groups * 4;
	size_t need = full_groups * 3 + (tail == 0 ? 0 : tail - 1);

	if (dst == NULL && dlen == 0) {
		*olen = need;
		return 0;
	}
	if (need > dlen) {
		*olen = need;
		return -ENOMEM;
	}

	size_t i = 0, j = 0;
	while (i + 4 <= full_groups * 4) {
		int v0 = b64_decode_char(src[i]);
		int v1 = b64_decode_char(src[i+1]);
		int v2 = b64_decode_char(src[i+2]);
		int v3 = b64_decode_char(src[i+3]);
		if ((v0 | v1 | v2 | v3) < 0) return -EINVAL;
		uint32_t v = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) |
			     ((uint32_t)v2 <<  6) |  (uint32_t)v3;
		dst[j++] = (v >> 16) & 0xff;
		dst[j++] = (v >>  8) & 0xff;
		dst[j++] =  v        & 0xff;
		i += 4;
	}
	if (tail >= 2) {
		int v0 = b64_decode_char(src[i]);
		int v1 = b64_decode_char(src[i+1]);
		if ((v0 | v1) < 0) return -EINVAL;
		dst[j++] = (uint8_t)((v0 << 2) | (v1 >> 4));
		if (tail == 3) {
			int v2 = b64_decode_char(src[i+2]);
			if (v2 < 0) return -EINVAL;
			dst[j++] = (uint8_t)((v1 << 4) | (v2 >> 2));
		}
	}
	*olen = j;
	return 0;
}
