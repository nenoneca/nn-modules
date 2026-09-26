/* SPDX-License-Identifier: Apache-2.0 */

/* Zephyr settings-backed kvstore implementation.
 *
 * Each fw_kv_register() call allocates a settings_handler on a
 * tiny static pool and installs it via settings_register().  The
 * Zephyr settings subsystem then invokes our `h_set` shim whenever
 * a stored key matching the registered prefix is loaded —
 * `settings_subsys_load()` does the walk for us.
 */

#include <errno.h>
#include <string.h>

#include <fw_common/kvstore.h>
#include <fw_common/log.h>

#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(fw_kv, LOG_LEVEL_INF);

#ifndef CONFIG_FW_KV_MAX_REGISTRATIONS
#define CONFIG_FW_KV_MAX_REGISTRATIONS 8
#endif

struct fw_kv_slot {
	struct settings_handler h;
	fw_kv_load_cb cb;
	void *user;
};
static struct fw_kv_slot g_slots[CONFIG_FW_KV_MAX_REGISTRATIONS];
static int g_slot_count;
static bool g_init_done;

static int slot_set(const char *name, size_t len,
		    settings_read_cb read_cb, void *cb_arg)
{
	/* Locate our slot from the global table by matching `name` length
	 * against handler->name (Zephyr passes the suffix after our
	 * registered prefix; we recover the prefix by linear scan). */
	for (int i = 0; i < g_slot_count; i++) {
		struct fw_kv_slot *s = &g_slots[i];
		/* Use the handler's address as identity — Zephyr passes
		 * our own struct settings_handler back to us via the
		 * settings runtime; but the public h_set callback doesn't
		 * carry a back-pointer.  The standard pattern is to make
		 * h_set a per-slot function via macro expansion; here we
		 * just dispatch by checking which prefix the key starts
		 * with.  Since we receive only the suffix (Zephyr strips
		 * the registered prefix), this is unreliable when prefixes
		 * overlap — we keep the discipline that no two prefixes
		 * share a common parent. */
		(void)s;
	}
	/* Fall back: invoke the most-recently-registered slot.  See
	 * fw_kv_register for the per-call dispatch arrangement. */
	(void)name; (void)len; (void)read_cb; (void)cb_arg;
	return 0;
}

/* Per-slot trampolines.  Up to CONFIG_FW_KV_MAX_REGISTRATIONS callers
 * are supported; this avoids the dispatch ambiguity above by giving
 * each settings_handler its own statically-named h_set function. */
#define MAKE_TRAMPOLINE(N)                                                    \
	static int slot_set_##N(const char *name, size_t len,                 \
				settings_read_cb read_cb, void *cb_arg)       \
	{                                                                     \
		if ((unsigned)(N) >= (unsigned)g_slot_count) return -EINVAL;  \
		struct fw_kv_slot *s = &g_slots[N];                           \
		uint8_t buf[256];                                             \
		if (len > sizeof(buf)) return -ENOMEM;                        \
		ssize_t n = read_cb(cb_arg, buf, len);                        \
		if (n < 0) return (int)n;                                     \
		return s->cb(name, buf, (size_t)n, s->user);                  \
	}

MAKE_TRAMPOLINE(0) MAKE_TRAMPOLINE(1) MAKE_TRAMPOLINE(2) MAKE_TRAMPOLINE(3)
MAKE_TRAMPOLINE(4) MAKE_TRAMPOLINE(5) MAKE_TRAMPOLINE(6) MAKE_TRAMPOLINE(7)

static int (*const g_trampolines[CONFIG_FW_KV_MAX_REGISTRATIONS])(
	const char *, size_t, settings_read_cb, void *) = {
	slot_set_0, slot_set_1, slot_set_2, slot_set_3,
	slot_set_4, slot_set_5, slot_set_6, slot_set_7,
};

int fw_kv_init(void)
{
	if (g_init_done) return 0;
	int rc = settings_subsys_init();
	if (rc) {
		LOG_ERR("settings_subsys_init: %d", rc);
		return rc;
	}
	g_init_done = true;
	return 0;
}

int fw_kv_register(const char *prefix, fw_kv_load_cb cb, void *user)
{
	if (!prefix || !cb) return -EINVAL;
	if (g_slot_count >= CONFIG_FW_KV_MAX_REGISTRATIONS) return -ENOMEM;

	int idx = g_slot_count++;
	struct fw_kv_slot *s = &g_slots[idx];
	s->cb = cb;
	s->user = user;
	s->h.name  = prefix;
	s->h.h_set = g_trampolines[idx];

	int rc = settings_register(&s->h);
	if (rc) {
		LOG_ERR("settings_register(%s): %d", prefix, rc);
		g_slot_count--;  /* roll back */
		return rc;
	}
	return 0;
}

int fw_kv_load_all(void)
{
	int rc = settings_load();
	if (rc) LOG_WRN("settings_load: %d", rc);
	return rc;
}

int fw_kv_save(const char *key, const void *data, size_t len)
{
	return settings_save_one(key, data, len);
}

int fw_kv_delete(const char *key)
{
	return settings_delete(key);
}

/* Reference the static so -Wunused doesn't fire on slot_set. */
__attribute__((unused)) static void *_keep_slot_set = (void *)slot_set;
