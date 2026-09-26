/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * nn_osal/byteorder.h — little/big-endian byte-array accessors.
 *
 * Wire-format codec helpers used across nn_proto, heartbeat, time_sync,
 * and field_relay.  Zephyr exposes the same shapes as sys_get_le16 /
 * sys_put_le32 etc. in <zephyr/sys/byteorder.h>; this header re-exports
 * them under the nn_osal_ namespace so libs stop reaching for the
 * vendor header directly.
 *
 * All operations are length-explicit (no struct casting) so they work
 * on unaligned pointers — that's the typical wire-format case.
 */

/* ── little-endian get ─────────────────────────────────────────── */

static inline uint16_t nn_osal_get_le16(const uint8_t src[2])
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static inline uint32_t nn_osal_get_le24(const uint8_t src[3])
{
    return  (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16);
}

static inline uint32_t nn_osal_get_le32(const uint8_t src[4])
{
    return  (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

static inline uint64_t nn_osal_get_le64(const uint8_t src[8])
{
    return  (uint64_t)src[0]
         | ((uint64_t)src[1] << 8)
         | ((uint64_t)src[2] << 16)
         | ((uint64_t)src[3] << 24)
         | ((uint64_t)src[4] << 32)
         | ((uint64_t)src[5] << 40)
         | ((uint64_t)src[6] << 48)
         | ((uint64_t)src[7] << 56);
}

/* ── little-endian put ─────────────────────────────────────────── */

static inline void nn_osal_put_le16(uint16_t v, uint8_t dst[2])
{
    dst[0] = (uint8_t)(v);
    dst[1] = (uint8_t)(v >> 8);
}

static inline void nn_osal_put_le24(uint32_t v, uint8_t dst[3])
{
    dst[0] = (uint8_t)(v);
    dst[1] = (uint8_t)(v >> 8);
    dst[2] = (uint8_t)(v >> 16);
}

static inline void nn_osal_put_le32(uint32_t v, uint8_t dst[4])
{
    dst[0] = (uint8_t)(v);
    dst[1] = (uint8_t)(v >> 8);
    dst[2] = (uint8_t)(v >> 16);
    dst[3] = (uint8_t)(v >> 24);
}

static inline void nn_osal_put_le64(uint64_t v, uint8_t dst[8])
{
    dst[0] = (uint8_t)(v);
    dst[1] = (uint8_t)(v >> 8);
    dst[2] = (uint8_t)(v >> 16);
    dst[3] = (uint8_t)(v >> 24);
    dst[4] = (uint8_t)(v >> 32);
    dst[5] = (uint8_t)(v >> 40);
    dst[6] = (uint8_t)(v >> 48);
    dst[7] = (uint8_t)(v >> 56);
}

/* ── big-endian (network byte order) ───────────────────────────── */

static inline uint16_t nn_osal_get_be16(const uint8_t src[2])
{
    return ((uint16_t)src[0] << 8) | (uint16_t)src[1];
}

static inline uint32_t nn_osal_get_be32(const uint8_t src[4])
{
    return  ((uint32_t)src[0] << 24)
         | ((uint32_t)src[1] << 16)
         | ((uint32_t)src[2] << 8)
         |  (uint32_t)src[3];
}

static inline void nn_osal_put_be16(uint16_t v, uint8_t dst[2])
{
    dst[0] = (uint8_t)(v >> 8);
    dst[1] = (uint8_t)(v);
}

static inline void nn_osal_put_be32(uint32_t v, uint8_t dst[4])
{
    dst[0] = (uint8_t)(v >> 24);
    dst[1] = (uint8_t)(v >> 16);
    dst[2] = (uint8_t)(v >> 8);
    dst[3] = (uint8_t)(v);
}

/* ── word-swap helpers (rare but occasionally needed) ──────────── */

static inline uint16_t nn_osal_bswap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t nn_osal_bswap32(uint32_t v)
{
    return  ((v & 0xFF000000u) >> 24)
         | ((v & 0x00FF0000u) >>  8)
         | ((v & 0x0000FF00u) <<  8)
         | ((v & 0x000000FFu) << 24);
}
