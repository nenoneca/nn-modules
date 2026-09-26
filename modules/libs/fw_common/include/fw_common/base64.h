/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * fw_common/base64.h — RFC 4648 standard base64 (no URL-safe variant).
 *
 * The API matches Zephyr's <zephyr/sys/base64.h> exactly so existing
 * code that includes either header behaves identically.
 *
 * `*olen` is set to the number of bytes actually written to `dst`.
 * Returns 0 on success, -ENOMEM if `dst` is too small.  Pass NULL `dst`
 * with `dlen == 0` to compute the required size in `*olen`.
 */

int fw_base64_encode(uint8_t *dst, size_t dlen, size_t *olen,
		     const uint8_t *src, size_t slen);

int fw_base64_decode(uint8_t *dst, size_t dlen, size_t *olen,
		     const uint8_t *src, size_t slen);

#ifndef __ZEPHYR__
/* Zephyr's API uses `base64_encode` / `base64_decode` without prefix;
 * mirror those names on non-Zephyr so hub_crypto.c (and other migrated
 * files) compile unchanged on Linux. */
static inline int base64_encode(uint8_t *dst, size_t dlen, size_t *olen,
				const uint8_t *src, size_t slen)
{
	return fw_base64_encode(dst, dlen, olen, src, slen);
}
static inline int base64_decode(uint8_t *dst, size_t dlen, size_t *olen,
				const uint8_t *src, size_t slen)
{
	return fw_base64_decode(dst, dlen, olen, src, slen);
}
#endif
