/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * fw_common/kvstore.h — tiny key-value store abstraction modelled
 * after Zephyr's settings subsystem, so on-Zephyr code maps 1:1.
 *
 * Usage (init order):
 *   1. fw_kv_init()              — once at startup
 *   2. fw_kv_register(prefix, cb, user)   — for each module that
 *      wants to receive load callbacks
 *   3. fw_kv_load_all()          — fires the callbacks once per
 *      stored key under each registered prefix
 *   4. fw_kv_save(key, data, len)         — overwrite/create a key
 *
 * Key strings are forward-slash-namespaced ("hub_crypto/dev_x25519_priv").
 * Callbacks receive only the suffix after `prefix/` (i.e. with the
 * registered prefix and its trailing slash stripped) — same as Zephyr's
 * settings handler convention.  Returning a non-zero value from the
 * callback signals an error to the loader.
 *
 * Zephyr backend: thin wrappers over settings_*.
 * Linux backend:  one binary file per key under
 *                 ${NN_GW_STATE_DIR:-$HOME/.local/state/nn-gw/kv}/<key>.
 */

typedef int (*fw_kv_load_cb)(const char *suffix,
			     const uint8_t *value, size_t value_len,
			     void *user);

int fw_kv_init(void);

int fw_kv_register(const char *prefix, fw_kv_load_cb cb, void *user);

int fw_kv_load_all(void);

int fw_kv_save(const char *key, const void *data, size_t len);

/* Remove a single key.  Returns 0 on success, -ENOENT if the key doesn't
 * exist (treated as success by most callers — wipe paths just want the
 * key gone). */
int fw_kv_delete(const char *key);
