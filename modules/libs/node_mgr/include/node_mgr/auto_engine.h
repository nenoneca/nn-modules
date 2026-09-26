/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * auto_engine — Distributed automation rule engine.
 *
 * Loads variable-length binary config compiled by the hub.
 * Evaluates triggers, sends CoAP notifications, executes actions.
 *
 * Binary format (variable-length, little-endian):
 *   Header: version(1) rule_count(1) reserved(2)
 *   Per rule:
 *     lps auto_id, uint8 role, uint8 op, uint8 notify_count,
 *     uint8 on_timeout, float threshold, float value,
 *     uint32 timeout_ms, lps field, [lps notify_name] * notify_count
 *   (lps = length-prefixed string: uint8 len + chars, no NUL)
 */

#define AUTO_MAX_ID     32
#define AUTO_MAX_FIELD  16
#define AUTO_MAX_NAME   48  /* IPv6 address string e.g. fd25:1022:fdbf:826f:3468:e1be:90d9:f07f */
#define AUTO_MAX_NOTIFY  4

#define AUTO_ROLE_TRIGGER   'T'
#define AUTO_ROLE_CONDITION 'C'
#define AUTO_ROLE_ACTION    'A'

#define AUTO_OP_ABOVE      0
#define AUTO_OP_BELOW      1
#define AUTO_OP_EQUALS     2
#define AUTO_OP_NOT_EQUALS 3
#define AUTO_OP_UNUSED     0xFF

/* Parsed rule (in-memory, fixed-size for array storage) */
struct auto_rule {
	char     auto_id[AUTO_MAX_ID];
	char     field[AUTO_MAX_FIELD];
	char     notify[AUTO_MAX_NOTIFY][AUTO_MAX_NAME];
	uint8_t  role;
	uint8_t  op;
	uint8_t  notify_count;
	uint8_t  on_timeout;
	float    threshold;
	float    value;
	uint32_t timeout_ms;
};

/* ── Field type for capability registry ────────────────────────────────────── */

#define AUTO_FIELD_TYPE_SENSOR   0  /* read-only input (temperature, humidity) */
#define AUTO_FIELD_TYPE_ACTUATOR 1  /* writable output (fan, led, relay) */

struct auto_field_desc {
	char    name[AUTO_MAX_FIELD];
	uint8_t type;       /* AUTO_FIELD_TYPE_SENSOR or _ACTUATOR */
	float   min_val;
	float   max_val;
};

#ifndef CONFIG_NODE_MGR_AUTO_MAX_FIELD_DESC
#define CONFIG_NODE_MGR_AUTO_MAX_FIELD_DESC 8
#endif

/* ── Public API ───────────────────────────────────────────────────────────── */

int   auto_engine_init(void);
int   auto_engine_load(const uint8_t *data, size_t len);
void  auto_engine_set_field(const char *field, float value);
float auto_engine_get_field(const char *field);

/* Call every few seconds (the app's sampling tick): pushes a snapshot of
 * every field that has a value to the hub's field cache the first time
 * after boot and then every SENSOR_PUSH_MAX_MS, so the webapp shows
 * values without a live mesh read even after a hub restart. */
void auto_engine_push_tick(void);
void  auto_engine_on_message(char *json);
int   auto_engine_rule_count(void);

/**
 * Register a field that this device supports.
 * Call at boot before auto_engine_init() or after.
 * The registered fields are reported in CoAP /info responses
 * so the hub compiler can validate automations.
 *
 * @param name   Field name (e.g. "temperature", "fan")
 * @param type   AUTO_FIELD_TYPE_SENSOR or AUTO_FIELD_TYPE_ACTUATOR
 * @param min_val Minimum value this field can have
 * @param max_val Maximum value this field can have
 * @return 0 on success, -ENOMEM if registry full
 */
int auto_engine_register_field(const char *name, uint8_t type,
			       float min_val, float max_val);

/** Get registered field descriptors (for CoAP /info). */
const struct auto_field_desc *auto_engine_get_field_descs(int *count);

/**
 * Actuator-write callback.
 * Invoked by auto_engine_set_field() *after* the value is cached, when
 * the named field has been registered with type AUTO_FIELD_TYPE_ACTUATOR.
 * Lets app code translate a "set led=1" cache update into an actual
 * GPIO toggle, PWM duty change, etc.
 *
 * The callback runs in the caller's thread context (typically the
 * CoAP server thread for hub-initiated writes, or whichever thread
 * pushed the value).  Keep it short — no blocking calls.
 */
typedef void (*auto_engine_actuator_cb)(const char *name, float value,
					 void *user);

/** Register an actuator callback for *name*.  Up to one cb per field;
 *  re-registering replaces the previous one.  Returns 0 on success,
 *  -ENOENT if the field isn't registered, -ENOTSUP if it's a SENSOR. */
int auto_engine_register_actuator_cb(const char *name,
				     auto_engine_actuator_cb cb, void *user);
