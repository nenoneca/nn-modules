/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DNS_CACHE_H
#define DNS_CACHE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Look up a cached name.
 *
 * @param name      Hostname to look up (e.g. "device2.local").
 * @param addr      Output: resolved IPv6 address (16 bytes, network order).
 * @param scope_id  Output: scope ID for link-local addresses (0 for global/ULA).
 * @return 0 on hit, -ENOENT on miss, -ESTALE if TTL expired (entry removed).
 *
 * In LRU mode (CONFIG_APP_DNS_CACHE_LRU=y) a hit updates the entry's
 * last-used timestamp, making it less likely to be evicted.
 */
int dns_cache_lookup(const char *name, uint8_t addr[16], uint32_t *scope_id);

/**
 * Insert or refresh a cache entry.
 *
 * If the name already exists the address is updated in place.
 * When the cache is full, the eviction policy (LRU or FIFO) selects
 * the entry to replace.
 */
void dns_cache_insert(const char *name, const uint8_t addr[16], uint32_t scope_id);

/** Remove a single entry by name (no-op if not present). */
void dns_cache_remove(const char *name);

/** Remove all entries. */
void dns_cache_clear(void);

/** Print all entries to the console. */
void dns_cache_dump(void);

#endif /* DNS_CACHE_H */
