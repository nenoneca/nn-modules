/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <fw_common/base64.h>
#include <fw_common/envelope.h>

/*
 * Tiny purpose-built parser for {"v":<n>,"epk":"<b64>","nonce":"<b64>",
 * "ct":"<b64>"}.  Whitespace-tolerant, key order independent, but
 * strictly enforces the 4 fields and their value types.
 */

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	return p;
}

/* Match a literal char after optional whitespace; advance *p past it. */
static int expect_ch(const char **p, char want)
{
	*p = skip_ws(*p);
	if (**p != want) return -1;
	(*p)++;
	return 0;
}

/* Parse a quoted JSON string into (start, end) — pointers into the
 * original buffer.  No unescaping (envelope values are base64 / ints,
 * no escape sequences expected). */
static int parse_string(const char **p, const char **start, size_t *len)
{
	*p = skip_ws(*p);
	if (**p != '"') return -1;
	(*p)++;
	*start = *p;
	while (**p && **p != '"') (*p)++;
	if (**p != '"') return -1;
	*len = (size_t)(*p - *start);
	(*p)++;
	return 0;
}

static int parse_int(const char **p, int *out)
{
	*p = skip_ws(*p);
	if (**p < '0' || **p > '9') return -1;
	int v = 0;
	while (**p >= '0' && **p <= '9') {
		v = v * 10 + (**p - '0');
		(*p)++;
	}
	*out = v;
	return 0;
}

int fw_envelope_v2_parse(const char *in, struct fw_envelope_v2 *out)
{
	if (!in || !out) return -EINVAL;
	memset(out, 0, sizeof(*out));

	const char *p = in;
	if (expect_ch(&p, '{') != 0) return -EINVAL;

	bool got_v = false, got_epk = false, got_nonce = false, got_ct = false;

	while (1) {
		p = skip_ws(p);
		if (*p == '}') { p++; break; }

		const char *key;
		size_t keylen;
		if (parse_string(&p, &key, &keylen) != 0) return -EINVAL;
		if (expect_ch(&p, ':') != 0) return -EINVAL;

		if (keylen == 1 && key[0] == 'v') {
			int v;
			if (parse_int(&p, &v) != 0 || v != 2) return -EINVAL;
			got_v = true;
		} else if (keylen == 3 && memcmp(key, "epk", 3) == 0) {
			const char *vs; size_t vlen;
			if (parse_string(&p, &vs, &vlen) != 0) return -EINVAL;
			size_t olen = 0;
			if (fw_base64_decode(out->epk, sizeof(out->epk), &olen,
					     (const uint8_t *)vs, vlen) != 0)
				return -EINVAL;
			if (olen != 32) return -EINVAL;
			got_epk = true;
		} else if (keylen == 5 && memcmp(key, "nonce", 5) == 0) {
			const char *vs; size_t vlen;
			if (parse_string(&p, &vs, &vlen) != 0) return -EINVAL;
			size_t olen = 0;
			if (fw_base64_decode(out->nonce, sizeof(out->nonce),
					     &olen,
					     (const uint8_t *)vs, vlen) != 0)
				return -EINVAL;
			if (olen != 12) return -EINVAL;
			got_nonce = true;
		} else if (keylen == 2 && memcmp(key, "ct", 2) == 0) {
			const char *vs; size_t vlen;
			if (parse_string(&p, &vs, &vlen) != 0) return -EINVAL;
			out->ct_b64     = (const uint8_t *)vs;
			out->ct_b64_len = vlen;
			got_ct = true;
		} else {
			/* Unknown field — skip its value (string only; the
			 * envelope schema doesn't use other types). */
			const char *vs; size_t vlen;
			if (parse_string(&p, &vs, &vlen) != 0) return -EINVAL;
		}

		p = skip_ws(p);
		if (*p == ',') { p++; continue; }
		if (*p == '}') { p++; break; }
		return -EINVAL;
	}

	if (!got_v || !got_epk || !got_nonce || !got_ct) return -EINVAL;
	return 0;
}

int fw_envelope_v2_build(char *out, size_t out_cap,
			 const uint8_t epk[32],
			 const uint8_t nonce[12],
			 const uint8_t *ct, size_t ct_len)
{
	/* Header + 3 base64 chunks + delimiters fits in this many bytes. */
	uint8_t b64_epk[48], b64_nonce[32];
	size_t  l_epk = 0, l_nonce = 0;

	int rc = fw_base64_encode(b64_epk, sizeof(b64_epk), &l_epk, epk, 32);
	if (rc != 0) return rc;
	rc = fw_base64_encode(b64_nonce, sizeof(b64_nonce), &l_nonce, nonce, 12);
	if (rc != 0) return rc;

	/* ct base64 may be large — write directly into the output buffer
	 * after the prefix. */
	const char prefix[] = "{\"v\":2,\"epk\":\"";
	const char mid1[]   = "\",\"nonce\":\"";
	const char mid2[]   = "\",\"ct\":\"";
	const char suffix[] = "\"}";

	size_t need = sizeof(prefix) - 1 + l_epk +
		      sizeof(mid1)   - 1 + l_nonce +
		      sizeof(mid2)   - 1;
	size_t ct_b64_max = ((ct_len + 2) / 3) * 4;
	need += ct_b64_max + sizeof(suffix); /* incl trailing NUL */

	if (out_cap < need) return -ENOMEM;

	size_t off = 0;
	memcpy(out + off, prefix, sizeof(prefix) - 1); off += sizeof(prefix) - 1;
	memcpy(out + off, b64_epk, l_epk);            off += l_epk;
	memcpy(out + off, mid1,    sizeof(mid1)   - 1); off += sizeof(mid1)   - 1;
	memcpy(out + off, b64_nonce, l_nonce);        off += l_nonce;
	memcpy(out + off, mid2,    sizeof(mid2)   - 1); off += sizeof(mid2)   - 1;

	size_t l_ct = 0;
	rc = fw_base64_encode((uint8_t *)(out + off), out_cap - off, &l_ct,
			      ct, ct_len);
	if (rc != 0) return rc;
	off += l_ct;

	memcpy(out + off, suffix, sizeof(suffix)); /* incl NUL */
	return 0;
}
