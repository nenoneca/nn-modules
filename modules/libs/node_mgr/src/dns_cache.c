/* SPDX-License-Identifier: Apache-2.0 */

/*
 * DNS cache — in-memory hostname → IPv6 address store.
 *
 * Capacity: CONFIG_NODE_MGR_DNS_CACHE_SIZE entries (default 4).
 * Eviction:
 *   LRU  (CONFIG_NODE_MGR_DNS_CACHE_LRU=y, default): evict the entry whose
 *         last_used timestamp is lowest (least recently accessed).
 *   FIFO (CONFIG_NODE_MGR_DNS_CACHE_LRU=n): last_used is only set at
 *         insertion time and never updated on lookup, so the same
 *         find_oldest() function naturally picks the earliest insert.
 * TTL:  CONFIG_NODE_MGR_DNS_CACHE_TTL_SEC seconds (0 = disabled).
 */

#include <nn_osal/osal.h>
#include <node_mgr/dns_cache.h>

#include <string.h>
#include <errno.h>

NN_OSAL_LOG_MODULE(dns_cache);

#define MAX_NAME_LEN  64

struct cache_entry {
	char          name[MAX_NAME_LEN];
	uint8_t       addr[16];    /* IPv6 address, network byte order */
	uint32_t      scope_id;   /* 0 for global/ULA; interface index for link-local */
	uint32_t      inserted_ms; /* nn_osal_uptime_ms_32() at insertion — for TTL check  */
	uint32_t      last_used_ms;/* updated on access in LRU mode, only on insert in FIFO */
	bool          valid;
};

static struct cache_entry g_table[CONFIG_NODE_MGR_DNS_CACHE_SIZE];
static K_MUTEX_DEFINE(g_lock);

/* ── helpers ────────────────────────────────────────────────────────── */

static bool entry_stale(const struct cache_entry *e)
{
#if CONFIG_NODE_MGR_DNS_CACHE_TTL_SEC > 0
	uint32_t age_ms = nn_osal_uptime_ms_32() - e->inserted_ms;
	return age_ms > ((uint32_t)CONFIG_NODE_MGR_DNS_CACHE_TTL_SEC * 1000U);
#else
	ARG_UNUSED(e);
	return false;
#endif
}

/* Return the victim slot: empty > stale > oldest last_used. */
static struct cache_entry *find_victim(void)
{
	struct cache_entry *victim = NULL;

	/* 1. prefer an empty slot */
	for (int i = 0; i < CONFIG_NODE_MGR_DNS_CACHE_SIZE; i++) {
		if (!g_table[i].valid) {
			return &g_table[i];
		}
	}

	/* 2. prefer an already-stale slot */
	for (int i = 0; i < CONFIG_NODE_MGR_DNS_CACHE_SIZE; i++) {
		if (entry_stale(&g_table[i])) {
			return &g_table[i];
		}
	}

	/* 3. evict by policy: LRU or FIFO both reduce to "oldest last_used" —
	 *    the difference is only whether last_used is bumped on every access
	 *    (LRU) or only at insert time (FIFO).                             */
	victim = &g_table[0];
	for (int i = 1; i < CONFIG_NODE_MGR_DNS_CACHE_SIZE; i++) {
		if (g_table[i].last_used_ms < victim->last_used_ms) {
			victim = &g_table[i];
		}
	}
	return victim;
}

/* ── public API ─────────────────────────────────────────────────────── */

int dns_cache_lookup(const char *name, uint8_t addr[16], uint32_t *scope_id)
{
	int rc = -ENOENT;

	k_mutex_lock(&g_lock, K_FOREVER);

	for (int i = 0; i < CONFIG_NODE_MGR_DNS_CACHE_SIZE; i++) {
		struct cache_entry *e = &g_table[i];

		if (!e->valid || strcmp(e->name, name) != 0) {
			continue;
		}

		if (entry_stale(e)) {
			NN_LOG_DBG("cache STALE: %s", name);
			e->valid = false;
			rc = -ESTALE;
			break;
		}

		memcpy(addr, e->addr, 16);
		*scope_id = e->scope_id;

		/* LRU: update timestamp on every access */
		if (IS_ENABLED(CONFIG_NODE_MGR_DNS_CACHE_LRU)) {
			e->last_used_ms = nn_osal_uptime_ms_32();
		}

		NN_LOG_DBG("cache HIT: %s", name);
		rc = 0;
		break;
	}

	k_mutex_unlock(&g_lock);
	return rc;
}

void dns_cache_insert(const char *name, const uint8_t addr[16], uint32_t scope_id)
{
	k_mutex_lock(&g_lock, K_FOREVER);

	/* If name already present, update in place. */
	for (int i = 0; i < CONFIG_NODE_MGR_DNS_CACHE_SIZE; i++) {
		struct cache_entry *e = &g_table[i];
		if (e->valid && strcmp(e->name, name) == 0) {
			memcpy(e->addr, addr, 16);
			e->scope_id    = scope_id;
			e->inserted_ms = nn_osal_uptime_ms_32();
			e->last_used_ms = e->inserted_ms;
			NN_LOG_DBG("cache UPDATE: %s", name);
			k_mutex_unlock(&g_lock);
			return;
		}
	}

	struct cache_entry *slot = find_victim();
	if (slot->valid) {
		NN_LOG_DBG("cache EVICT: %s (policy: %s)",
			slot->name,
			IS_ENABLED(CONFIG_NODE_MGR_DNS_CACHE_LRU) ? "LRU" : "FIFO");
	}

	strncpy(slot->name, name, MAX_NAME_LEN - 1);
	slot->name[MAX_NAME_LEN - 1] = '\0';
	memcpy(slot->addr, addr, 16);
	slot->scope_id    = scope_id;
	slot->inserted_ms = nn_osal_uptime_ms_32();
	slot->last_used_ms = slot->inserted_ms;
	slot->valid       = true;

	NN_LOG_DBG("cache INSERT: %s", name);
	k_mutex_unlock(&g_lock);
}

void dns_cache_remove(const char *name)
{
	k_mutex_lock(&g_lock, K_FOREVER);
	for (int i = 0; i < CONFIG_NODE_MGR_DNS_CACHE_SIZE; i++) {
		if (g_table[i].valid && strcmp(g_table[i].name, name) == 0) {
			g_table[i].valid = false;
			break;
		}
	}
	k_mutex_unlock(&g_lock);
}

void dns_cache_clear(void)
{
	k_mutex_lock(&g_lock, K_FOREVER);
	memset(g_table, 0, sizeof(g_table));
	k_mutex_unlock(&g_lock);
}

void dns_cache_dump(void)
{
	char addr_str[NN_OSAL_INET6_ADDRSTRLEN];
	uint32_t now = nn_osal_uptime_ms_32();
	int count = 0;

	k_mutex_lock(&g_lock, K_FOREVER);

	printk("DNS cache  [size=%d, policy=%s, TTL=%ds]\n",
	       CONFIG_NODE_MGR_DNS_CACHE_SIZE,
	       IS_ENABLED(CONFIG_NODE_MGR_DNS_CACHE_LRU) ? "LRU" : "FIFO",
	       CONFIG_NODE_MGR_DNS_CACHE_TTL_SEC);

	for (int i = 0; i < CONFIG_NODE_MGR_DNS_CACHE_SIZE; i++) {
		struct cache_entry *e = &g_table[i];
		if (!e->valid) {
			continue;
		}
		nn_osal_inet_ntop6(e->addr, addr_str, sizeof(addr_str));
		uint32_t age_s = (now - e->inserted_ms) / 1000U;
		bool stale = entry_stale(e);
		printk("  [%d] %-30s  %s  scope=%u  age=%us%s\n",
		       i, e->name, addr_str, e->scope_id, age_s,
		       stale ? "  STALE" : "");
		count++;
	}

	if (count == 0) {
		printk("  (empty)\n");
	}

	k_mutex_unlock(&g_lock);
}
