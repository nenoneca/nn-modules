/* SPDX-License-Identifier: Apache-2.0 */

/*
 * auto_engine.c — Distributed automation rule engine.
 *
 * Evaluates triggers on set_field(), sends CoAP NON-CON POST /auto
 * notifications to other devices, receives trigger/condition messages,
 * executes actions when all conditions are met within timeout.
 */

#include <nn_osal/osal.h>
#include <node_mgr/auto_engine.h>
#include <node_mgr/nn_group.h>
#include <node_mgr/nn_proto_client.h>
#include <nn_proto/nn_proto.h>

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
/* <zephyr/net/hostname.h> removed — not referenced; mDNS hostname now
 * via nn_pal_mdns_set_hostname(). */
#include <nn_pal/openthread.h>

#include <openthread/instance.h>
#include <openthread/ip6.h>
#include <openthread/link.h>
#include <openthread/message.h>
#include <openthread/thread.h>
#include <openthread/udp.h>

NN_OSAL_LOG_MODULE(auto_engine);

/* ── Configuration ────────────────────────────────────────────────────────── */

#ifndef CONFIG_NODE_MGR_AUTO_MAX_RULES
#define CONFIG_NODE_MGR_AUTO_MAX_RULES 16
#endif
#ifndef CONFIG_NODE_MGR_AUTO_MAX_PENDING
#define CONFIG_NODE_MGR_AUTO_MAX_PENDING 4
#endif
#ifndef CONFIG_NODE_MGR_AUTO_MAX_FIELDS
#define CONFIG_NODE_MGR_AUTO_MAX_FIELDS 8
#endif

/* Maximum on-wire AUTO_NOTIFY JSON body — sized to fit a typical rule
 * notification (auto_id + tid + field + value).  Same value as the
 * legacy CoAP path used. */
#define AUTO_BODY_MAX  256
#define HDR_SIZE       4

/* ── State ────────────────────────────────────────────────────────────────── */

static struct auto_rule g_rules[CONFIG_NODE_MGR_AUTO_MAX_RULES];
static int g_rule_count;

struct field_entry {
	char  name[AUTO_MAX_FIELD];
	float value;
};
static struct field_entry g_fields[CONFIG_NODE_MGR_AUTO_MAX_FIELDS];
static int g_field_count;

/* Field capability registry (for CoAP /info) */
static struct auto_field_desc g_field_descs[CONFIG_NODE_MGR_AUTO_MAX_FIELD_DESC];
static int g_field_desc_count;

/* Actuator-write callbacks, one per registered field index. */
static struct {
	auto_engine_actuator_cb cb;
	void                   *user;
} g_actuator_cbs[CONFIG_NODE_MGR_AUTO_MAX_FIELD_DESC];

static uint16_t g_tid_counter;

/* ── Pending actions (waiting for conditions) ─────────────────────────────── */

struct pending_action {
	bool     active;
	char     auto_id[AUTO_MAX_ID];
	uint16_t tid;
	int      cond_count;
	float    cond_values[AUTO_MAX_NOTIFY];
	bool     cond_received[AUTO_MAX_NOTIFY];
	const struct auto_rule *rule;  /* action rule pointer */
	struct k_work_delayable timeout_work;
};

static struct pending_action g_pending[CONFIG_NODE_MGR_AUTO_MAX_PENDING];

/* ── Field storage ────────────────────────────────────────────────────────── */

static float *field_ptr(const char *name)
{
	for (int i = 0; i < g_field_count; i++) {
		if (strncmp(g_fields[i].name, name, AUTO_MAX_FIELD) == 0) {
			return &g_fields[i].value;
		}
	}
	if (g_field_count < CONFIG_NODE_MGR_AUTO_MAX_FIELDS) {
		struct field_entry *e = &g_fields[g_field_count++];
		strncpy(e->name, name, AUTO_MAX_FIELD - 1);
		e->value = NAN;
		return &e->value;
	}
	return NULL;
}

float auto_engine_get_field(const char *field)
{
	float *p = field_ptr(field);
	return p ? *p : NAN;
}

/* ── Operator evaluation ──────────────────────────────────────────────────── */

static bool evaluate_op(float value, uint8_t op, float threshold)
{
	switch (op) {
	case AUTO_OP_ABOVE:      return value > threshold;
	case AUTO_OP_BELOW:      return value < threshold;
	case AUTO_OP_EQUALS:     return value == threshold;
	case AUTO_OP_NOT_EQUALS: return value != threshold;
	default:                 return false;
	}
}

/* ── D2D notification sender ─────────────────────────────────────── */

/* Thin wrapper around the generic nn_proto_client reliable layer.
 * AUTO_NOTIFY → AUTO_NOTIFY_ACK pair; library handles tid management,
 * burst-fire, deadline + ACK matching, RTT estimation, retry rounds. */
static void notify_enqueue(const char *target_ipv6, const char *json_payload)
{
	uint8_t peer[16];
	int rv = nn_osal_inet_pton6(target_ipv6, peer);
	if (rv) {
		NN_LOG_WRN("notify: invalid IPv6: %s", target_ipv6);
		return;
	}
	size_t body_len = strlen(json_payload);

	/* Phase 4: group-sealed cascade — no ECDSA sign per target, and
	 * the receiver actually authenticates (legacy D2D never did).
	 * Fall back to the signed legacy path until the hub has pushed
	 * a group key this boot. */
	if (nn_group_ready()) {
		uint8_t sealed[AUTO_BODY_MAX + NN_GROUP_OVERHEAD];
		size_t  sealed_len = 0;
		rv = nn_group_seal(nn_proto_client_get_device_id(),
				   NN_PROTO_CMD_AUTO_NOTIFY_S,
				   (const uint8_t *)json_payload, body_len,
				   sealed, sizeof sealed, &sealed_len);
		if (rv == 0) {
			rv = nn_proto_client_send_d2d_reliable_ns(peer,
					NN_PROTO_CMD_AUTO_NOTIFY_S,
					NN_PROTO_CMD_AUTO_NOTIFY_ACK,
					0, sealed, sealed_len, NULL, NULL);
			if (rv) {
				NN_LOG_WRN("notify_s: %s rv=%d",
					   target_ipv6, rv);
			}
			return;
		}
		NN_LOG_WRN("group seal rv=%d — legacy notify", rv);
	}

	rv = nn_proto_client_send_d2d_reliable(peer,
					       NN_PROTO_CMD_AUTO_NOTIFY,
					       NN_PROTO_CMD_AUTO_NOTIFY_ACK,
					       0,  /* auto-generated tid */
					       (const uint8_t *)json_payload,
					       body_len, NULL, NULL);
	if (rv) {
		NN_LOG_WRN("notify: send_d2d_reliable %s rv=%d", target_ipv6, rv);
	}
}

static char g_my_mleid[48]; /* our ML-EID, cached at init */

/* Forward decl: invoked locally from a trigger fire so same-device
 * actions execute without depending on a CoAP self-loop (which Thread
 * doesn't route).  Defined further down. */
static void handle_trigger(const char *auto_id, uint16_t tid,
			   const char *field, float value);

/* Lazily populate g_my_mleid the first time we need it.  auto_engine_init
 * runs before Thread is up under CONFIG_OPENTHREAD_MANUAL_START=y, so the
 * boot-time lookup typically returns the unspecified address and leaves
 * g_my_mleid empty.  Without this refresh, the self-detection in
 * notify_targets below never matches → every cascade rule that names
 * `<this device>` as one of its notify targets tries to send AUTO_NOTIFY
 * to its own ML-EID over D2D, which the reliable layer holds for 16+ s
 * each before timing out.  With 6 in-flight slots and triggers every
 * ~30 s the slot pool exhausts and notify_enqueue returns NO_SLOT (-2)
 * to every cross-device target. */
static bool mleid_is_real(const char *s)
{
	/* Empty *or* the unspecified-address sentinel "0:0:0:0:0:0:0:0"
	 * means we haven't actually been assigned a Mesh-Local address
	 * yet.  The early init at boot populates g_my_mleid with
	 * "0:0:0:0:0:0:0:0" because Thread is not yet attached under
	 * CONFIG_OPENTHREAD_MANUAL_START=y, and the previous early-out
	 * `if (g_my_mleid[0]) return;` was true for that ('0' is a
	 * printable character) — so the lazy refresh never ran. */
	return s[0] != '\0' && strcmp(s, "0:0:0:0:0:0:0:0") != 0;
}

static void refresh_my_mleid(void)
{
	if (mleid_is_real(g_my_mleid)) return;
	otInstance *inst = (otInstance *)nn_pal_ot_instance();
	if (!inst) return;
	nn_pal_ot_mutex_lock();
	const otIp6Address *mleid = otThreadGetMeshLocalEid(inst);
	if (mleid) {
		char buf[48];
		snprintf(buf, sizeof(buf),
			 "%x:%x:%x:%x:%x:%x:%x:%x",
			 nn_osal_get_be16(&mleid->mFields.m8[0]),
			 nn_osal_get_be16(&mleid->mFields.m8[2]),
			 nn_osal_get_be16(&mleid->mFields.m8[4]),
			 nn_osal_get_be16(&mleid->mFields.m8[6]),
			 nn_osal_get_be16(&mleid->mFields.m8[8]),
			 nn_osal_get_be16(&mleid->mFields.m8[10]),
			 nn_osal_get_be16(&mleid->mFields.m8[12]),
			 nn_osal_get_be16(&mleid->mFields.m8[14]));
		/* Skip the unspecified-address sentinel that OT returns when
		 * Thread is not yet attached. */
		if (strcmp(buf, "0:0:0:0:0:0:0:0") != 0) {
			strncpy(g_my_mleid, buf, sizeof(g_my_mleid) - 1);
			NN_LOG_INF("auto_engine: ML-EID cached lazily: %s",
				   g_my_mleid);
		}
	}
	nn_pal_ot_mutex_unlock();
}

/* Return true if `target` (string form, e.g. "fd44:8b73:6d00:1:...") is
 * any of OUR unicast addresses — ML-EID, OMR-derived SLAAC, link-local.
 *
 * Comparing only against `g_my_mleid` is fragile: the hub's coap_addr
 * cache can hold a previously-seen ML-EID for this device that no
 * longer matches the value Thread is using now (the random IID isn't
 * persisted across reboot under our settings backend, so it gets
 * regenerated on every cold boot).  When that drift happens, the
 * rule blob the compiler emits embeds the old string; strcmp fails;
 * we end up enqueuing AUTO_NOTIFY D2D to our own old ML-EID, which
 * lives on no node anywhere and burns a reliable-layer slot for the
 * full retry budget before giving up.  With INFLIGHT=6 and triggers
 * every ~30 s, the slot pool starves and the cross-device targets
 * also fail with NO_SLOT — making it look like the mesh is broken
 * when it's actually just self-spam.
 *
 * Iterating otIp6GetUnicastAddresses sidesteps the whole cache-
 * coherence question by asking OT "what IS on this interface right
 * now."  Cheap call (linked list of <16 entries normally). */
static bool target_is_self(const char *target)
{
	if (mleid_is_real(g_my_mleid) && strcmp(target, g_my_mleid) == 0) {
		return true;  /* fast path — common case */
	}

	otInstance *inst = (otInstance *)nn_pal_ot_instance();
	if (!inst) return false;

	otIp6Address tgt_addr;
	if (otIp6AddressFromString(target, &tgt_addr) != OT_ERROR_NONE) {
		return false;
	}

	nn_pal_ot_mutex_lock();
	bool match = false;
	for (const otNetifAddress *a = otIp6GetUnicastAddresses(inst);
	     a != NULL; a = a->mNext) {
		if (memcmp(&a->mAddress, &tgt_addr, sizeof(tgt_addr)) == 0) {
			match = true;
			break;
		}
	}
	nn_pal_ot_mutex_unlock();
	return match;
}

static void notify_targets(const struct auto_rule *rule,
			   const char *json_payload)
{
	refresh_my_mleid();
	for (int i = 0; i < rule->notify_count && i < AUTO_MAX_NOTIFY; i++) {
		const char *target = rule->notify[i];
		if (target[0] == '\0') {
			continue;
		}

		if (target_is_self(target)) {
			char json_copy[AUTO_BODY_MAX];
			strncpy(json_copy, json_payload, sizeof(json_copy) - 1);
			json_copy[sizeof(json_copy) - 1] = '\0';
			auto_engine_on_message(json_copy);
		} else {
			notify_enqueue(target, json_payload);
		}
	}
}

/* ── Field push policy ─────────────────────────────────────────────────────
 *
 * Actuators push on every change (they are rare and each one is an
 * automation outcome somebody is watching).  Sensors sample every few
 * seconds and would flood the mesh, so they push when the value moved by
 * more than SENSOR_PUSH_DEADBAND (a fraction of the field's range) and at
 * least SENSOR_PUSH_MIN_MS since the last push -- and unconditionally
 * every SENSOR_PUSH_MAX_MS so a hub that restarted (RAM cache) or missed
 * an event still shows a value within that window. */
#define SENSOR_PUSH_DEADBAND  0.01f      /* 1 % of (max - min) */
#define SENSOR_PUSH_MIN_MS    30000      /* 30 s between pushes when moving */
#define SENSOR_PUSH_MAX_MS    300000     /* 5 min heartbeat push when still */
/* A push before the mesh is attached is lost after its retries; the
 * boot-time sample is therefore not pushed and auto_engine_push_tick()
 * sends the first snapshot of every field once the device has had time
 * to attach.  The tick also covers fields set_field() never touches
 * again after boot (a button, an actuator nobody wrote) which a hub that
 * restarted -- RAM cache -- would otherwise never learn until they change. */
#define SNAPSHOT_FIRST_MS     60000

static int64_t g_last_push_ms[CONFIG_NODE_MGR_AUTO_MAX_FIELD_DESC];

static bool field_push_due(int idx, float old, float value)
{
	const struct auto_field_desc *d = &g_field_descs[idx];
	bool changed = isnan(old) || old != value;

	if (d->type == AUTO_FIELD_TYPE_ACTUATOR) {
		return changed;
	}
	int64_t now = nn_osal_uptime_ms();
	int64_t since = now - g_last_push_ms[idx];
	if (g_last_push_ms[idx] == 0) {
		return now >= SNAPSHOT_FIRST_MS; /* else the tick sends it */
	}
	if (since >= SENSOR_PUSH_MAX_MS) {
		return true;
	}
	if (since < SENSOR_PUSH_MIN_MS || !changed) {
		return false;
	}
	float span = d->max_val - d->min_val;
	float band = span > 0 ? span * SENSOR_PUSH_DEADBAND : 0;
	return isnan(old) || fabsf(value - old) > band;
}

/* ── Trigger evaluation ───────────────────────────────────────────────────── */

/* Send one {"t":"fld"} event for desc i and stamp its push time. */
static void push_field_event(int i, const char *field, float value)
{
	g_last_push_ms[i] = nn_osal_uptime_ms();
	char ev[96];
	float av = value < 0 ? -value : value;
	unsigned ip = (unsigned)av;
	unsigned fp = (unsigned)((av - (float)ip) * 1000.0f + 0.5f);
	if (fp >= 1000) { ip++; fp -= 1000; }
	int  el = snprintf(ev, sizeof ev,
			   "{\"t\":\"fld\",\"n\":\"%.*s\","
			   "\"v\":%s%u.%03u}",
			   AUTO_MAX_FIELD, field,
			   value < 0 ? "-" : "", ip, fp);
	if (el > 0 && el < (int)sizeof ev) {
		/* Reliable + unsigned: the retry/ACK layer
		 * (5 rounds, RTT-adaptive backoff) is what
		 * makes this survive a weak radio — a blind
		 * double-send still lost events on the
		 * worst link.  Cheap now: zero-sig frames,
		 * hub acks AUTO_EVENT_ACK, cache overwrite
		 * is idempotent so duplicates are free. */
		(void)nn_proto_client_send_d2h_reliable_ns(
			NN_PROTO_CMD_AUTO_EVENT,
			NN_PROTO_CMD_AUTO_EVENT_ACK,
			0, (const uint8_t *)ev, (size_t)el,
			NULL, NULL);
	}
}

/* Trigger evaluation for a field that just changed. */
static void eval_triggers(const char *field, float old, float value)
{
	for (int i = 0; i < g_rule_count; i++) {
		struct auto_rule *r = &g_rules[i];

		if (r->role != AUTO_ROLE_TRIGGER) {
			continue;
		}
		if (strncmp(r->field, field, AUTO_MAX_FIELD) != 0) {
			continue;
		}
		if (!evaluate_op(value, r->op, r->threshold)) {
			continue;
		}
		/* Edge detection: skip if old value already met the condition */
		if (!isnan(old) && evaluate_op(old, r->op, r->threshold)) {
			continue;
		}

		g_tid_counter++;
		NN_LOG_INF("TRIGGER %s tid=%u %s=%.1f",
			r->auto_id, g_tid_counter, field, (double)value);

		/* Build JSON notification */
		char json[128];
		snprintf(json, sizeof(json),
			 "{\"t\":\"trg\",\"a\":\"%.*s\",\"tid\":%u,"
			 "\"f\":\"%.*s\",\"v\":%.2f}",
			 AUTO_MAX_ID, r->auto_id, g_tid_counter,
			 AUTO_MAX_FIELD, field, (double)value);

		notify_targets(r, json);

		/* Same-device action: dispatch locally too — Thread doesn't
		 * loop CoAP /auto back to self, so an A-role rule on the
		 * trigger device would otherwise never fire. */
		handle_trigger(r->auto_id, g_tid_counter, field, value);
	}
}

void auto_engine_set_field(const char *field, float value)
{
	float *p = field_ptr(field);
	if (!p) {
		return;
	}
	float old = *p;
	*p = value;

	/* Fire actuator callback (if any) so app code can drive HW. */
	for (int i = 0; i < g_field_desc_count; i++) {
		if (strncmp(g_field_descs[i].name, field, AUTO_MAX_FIELD) != 0) {
			continue;
		}
		if (g_field_descs[i].type == AUTO_FIELD_TYPE_ACTUATOR &&
		    g_actuator_cbs[i].cb) {
			g_actuator_cbs[i].cb(field, value, g_actuator_cbs[i].user);
		}
		if (field_push_due(i, old, value)) {
			/* The field changed — an actuator from ANY source
			 * (hub write, local rule action, D2D notify), or a
			 * sensor by more than its dead-band.  Push a
			 * fire-and-forget AUTO_EVENT so the hub's field
			 * cache (and the webapp cards) track reality
			 * without polling the mesh.  Sensors are rate-
			 * limited (see field_push_due): without any push
			 * a sensor value only ever reached the webapp
			 * through a live read per page load, which on a
			 * busy mesh is seconds of "…" ending in "—". */
			push_field_event(i, field, value);
		}
		break;
	}
	eval_triggers(field, old, value);
}

void auto_engine_push_tick(void)
{
	int64_t now = nn_osal_uptime_ms();
	for (int i = 0; i < g_field_desc_count; i++) {
		float *p = field_ptr(g_field_descs[i].name);
		if (!p || isnan(*p)) {
			continue;               /* never had a value: nothing to say */
		}
		int64_t last = g_last_push_ms[i];
		bool due = (last == 0) ? (now >= SNAPSHOT_FIRST_MS)
				       : (now - last >= SENSOR_PUSH_MAX_MS);
		if (due) {
			push_field_event(i, g_field_descs[i].name, *p);
		}
	}
}

/* ── Pending action timeout handler ───────────────────────────────────────── */

static void pending_timeout_handler(struct k_work *work)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(work);
	struct pending_action *pa = CONTAINER_OF(dw, struct pending_action,
						 timeout_work);
	if (!pa->active) {
		return;
	}

	NN_LOG_INF("ACTION %s tid=%u TIMEOUT", pa->auto_id, pa->tid);

	/* Evaluate with on_timeout defaults for missing conditions */
	const struct auto_rule *r = pa->rule;
	bool all_ok = true;

	for (int i = 0; i < pa->cond_count; i++) {
		if (pa->cond_received[i]) {
			if (!evaluate_op(pa->cond_values[i], r->op,
					 r->threshold)) {
				all_ok = false;
				break;
			}
		} else {
			/* Not received — use on_timeout */
			if (!r->on_timeout) {
				all_ok = false;
				break;
			}
		}
	}

	if (all_ok) {
		NN_LOG_INF("ACTION %s tid=%u EXEC → %.*s=%.1f",
			pa->auto_id, pa->tid,
			AUTO_MAX_FIELD, r->field, (double)r->value);
		/* Use set_field so actuator callbacks fire and rule cascades
		 * (a downstream trigger can re-evaluate on this write). */
		auto_engine_set_field(r->field, r->value);
	} else {
		NN_LOG_INF("ACTION %s tid=%u SKIPPED (condition not met)",
			pa->auto_id, pa->tid);
	}

	pa->active = false;
}

/* ── Incoming message handlers ────────────────────────────────────────────── */

static void handle_trigger(const char *auto_id, uint16_t tid,
			   const char *field, float value)
{
	/* Condition role: push our field value to action devices */
	for (int i = 0; i < g_rule_count; i++) {
		struct auto_rule *r = &g_rules[i];
		if (r->role != AUTO_ROLE_CONDITION) {
			continue;
		}
		if (strncmp(r->auto_id, auto_id, AUTO_MAX_ID) != 0) {
			continue;
		}

		float cond_val = auto_engine_get_field(r->field);
		if (isnan(cond_val)) {
			cond_val = 0;
		}

		NN_LOG_INF("CONDITION %s tid=%u %.*s=%.1f",
			auto_id, tid, AUTO_MAX_FIELD, r->field,
			(double)cond_val);

		char json[128];
		snprintf(json, sizeof(json),
			 "{\"t\":\"cond\",\"a\":\"%.*s\",\"tid\":%u,"
			 "\"f\":\"%.*s\",\"v\":%.2f}",
			 AUTO_MAX_ID, auto_id, tid,
			 AUTO_MAX_FIELD, r->field, (double)cond_val);

		notify_targets(r, json);
	}

	/* Action role: start collecting conditions */
	for (int i = 0; i < g_rule_count; i++) {
		struct auto_rule *r = &g_rules[i];
		if (r->role != AUTO_ROLE_ACTION) {
			continue;
		}
		if (strncmp(r->auto_id, auto_id, AUTO_MAX_ID) != 0) {
			continue;
		}

		/* No conditions → execute immediately */
		if (r->notify_count == 0 || r->op == AUTO_OP_UNUSED) {
			NN_LOG_INF("ACTION %s tid=%u EXEC (unconditional) → "
				"%.*s=%.1f",
				auto_id, tid, AUTO_MAX_FIELD, r->field,
				(double)r->value);
			/* Use set_field so actuator callbacks fire and rule
			 * cascades (a downstream trigger can re-evaluate). */
			auto_engine_set_field(r->field, r->value);
			continue;
		}

		/* Find a free pending slot */
		struct pending_action *pa = NULL;
		for (int j = 0; j < CONFIG_NODE_MGR_AUTO_MAX_PENDING; j++) {
			if (!g_pending[j].active) {
				pa = &g_pending[j];
				break;
			}
		}
		if (!pa) {
			NN_LOG_WRN("No free pending slots for %s", auto_id);
			continue;
		}

		memset(pa, 0, sizeof(*pa));
		pa->active = true;
		strncpy(pa->auto_id, auto_id, AUTO_MAX_ID);
		pa->tid = tid;
		pa->cond_count = r->notify_count;
		pa->rule = r;
		k_work_init_delayable(&pa->timeout_work,
				      pending_timeout_handler);
		k_work_schedule(&pa->timeout_work,
				K_MSEC(r->timeout_ms));

		NN_LOG_INF("ACTION %s tid=%u waiting for %d conditions (%u ms)",
			auto_id, tid, pa->cond_count, r->timeout_ms);
	}
}

static void handle_condition(const char *auto_id, uint16_t tid,
			     const char *field, float value)
{
	for (int i = 0; i < CONFIG_NODE_MGR_AUTO_MAX_PENDING; i++) {
		struct pending_action *pa = &g_pending[i];
		if (!pa->active) {
			continue;
		}
		if (strncmp(pa->auto_id, auto_id, AUTO_MAX_ID) != 0 ||
		    pa->tid != tid) {
			continue;
		}

		/* Match condition by index in notify list */
		const struct auto_rule *r = pa->rule;
		for (int j = 0; j < pa->cond_count; j++) {
			/* The notify slot names are condition device names;
			 * we match by field name from the condition message */
			if (!pa->cond_received[j]) {
				pa->cond_values[j] = value;
				pa->cond_received[j] = true;
				break;
			}
		}

		/* Check if all conditions received */
		bool all_received = true;
		for (int j = 0; j < pa->cond_count; j++) {
			if (!pa->cond_received[j]) {
				all_received = false;
				break;
			}
		}

		if (all_received) {
			/* Cancel timeout and evaluate */
			k_work_cancel_delayable(&pa->timeout_work);

			bool all_ok = true;
			for (int j = 0; j < pa->cond_count; j++) {
				if (!evaluate_op(pa->cond_values[j],
						 r->op, r->threshold)) {
					all_ok = false;
					break;
				}
			}

			if (all_ok) {
				NN_LOG_INF("ACTION %s tid=%u EXEC → %.*s=%.1f",
					pa->auto_id, pa->tid,
					AUTO_MAX_FIELD, r->field,
					(double)r->value);
				/* Use set_field so actuator callbacks fire and
				 * rule cascades (downstream triggers run). */
				auto_engine_set_field(r->field, r->value);
			} else {
				NN_LOG_INF("ACTION %s tid=%u SKIPPED",
					pa->auto_id, pa->tid);
			}
			pa->active = false;
		}
		return;
	}
}

/* ── JSON message parser (incoming CoAP /auto) ────────────────────────────── */

/* Simple field extraction — avoids full JSON parser dependency */
static const char *json_str(const char *json, const char *key,
			    char *out, size_t out_len)
{
	char search[32];
	snprintf(search, sizeof(search), "\"%s\":\"", key);
	const char *p = strstr(json, search);
	if (!p) {
		return NULL;
	}
	p += strlen(search);
	const char *end = strchr(p, '"');
	if (!end) {
		return NULL;
	}
	size_t len = MIN((size_t)(end - p), out_len - 1);
	memcpy(out, p, len);
	out[len] = '\0';
	return out;
}

static float json_float(const char *json, const char *key)
{
	char search[32];
	snprintf(search, sizeof(search), "\"%s\":", key);
	const char *p = strstr(json, search);
	if (!p) {
		return NAN;
	}
	p += strlen(search);
	return strtof(p, NULL);
}

static int json_int(const char *json, const char *key)
{
	char search[32];
	snprintf(search, sizeof(search), "\"%s\":", key);
	const char *p = strstr(json, search);
	if (!p) {
		return -1;
	}
	p += strlen(search);
	return atoi(p);
}

void auto_engine_on_message(char *json)
{
	char type[8], auto_id[AUTO_MAX_ID + 1], field[AUTO_MAX_FIELD + 1];

	if (!json_str(json, "t", type, sizeof(type))) {
		return;
	}
	if (!json_str(json, "a", auto_id, sizeof(auto_id))) {
		return;
	}

	int tid = json_int(json, "tid");
	json_str(json, "f", field, sizeof(field));
	float value = json_float(json, "v");

	if (strcmp(type, "trg") == 0) {
		handle_trigger(auto_id, (uint16_t)tid, field, value);
	} else if (strcmp(type, "cond") == 0) {
		handle_condition(auto_id, (uint16_t)tid, field, value);
	}
}

/* ── Config loading ───────────────────────────────────────────────────────── */

/* Read a length-prefixed string from binary buffer */
static int read_lps(const uint8_t *buf, size_t buf_len, size_t *pos,
		    char *out, size_t out_cap)
{
	if (*pos >= buf_len) {
		return -EINVAL;
	}
	uint8_t slen = buf[*pos];
	(*pos)++;
	if (*pos + slen > buf_len) {
		return -EINVAL;
	}
	size_t copy = MIN(slen, out_cap - 1);
	memcpy(out, buf + *pos, copy);
	out[copy] = '\0';
	*pos += slen;
	return 0;
}

/* Largest rule blob we persist.  The hub-compiled blobs observed in
 * the field are ~125–412 B; 768 leaves comfortable headroom and matches
 * the reliable-layer body cap. */
#define AUTO_BLOB_MAX  768
#define AUTO_KV_PREFIX "auto"
#define AUTO_KV_KEY    AUTO_KV_PREFIX "/blob"

/* Parse a rule blob into g_rules[] (RAM).  Shared by the AUTO_PUSH path
 * (auto_engine_load, which also persists) and the boot-time NVS replay
 * (auto_blob_kv_load, which must NOT re-persist). */
static int auto_engine_apply_blob(const uint8_t *data, size_t len)
{
	if (len < HDR_SIZE) {
		return -EINVAL;
	}

	uint8_t version = data[0];
	uint8_t count = data[1];

	if (version != 1) {
		NN_LOG_ERR("Unknown config version %u", version);
		return -ENOTSUP;
	}

	if (count > CONFIG_NODE_MGR_AUTO_MAX_RULES) {
		NN_LOG_WRN("Rule count %u exceeds max %d, truncating",
			count, CONFIG_NODE_MGR_AUTO_MAX_RULES);
		count = CONFIG_NODE_MGR_AUTO_MAX_RULES;
	}

	size_t pos = HDR_SIZE;
	int loaded = 0;

	for (int i = 0; i < count && pos < len; i++) {
		struct auto_rule *r = &g_rules[i];
		memset(r, 0, sizeof(*r));

		/* auto_id (lps) */
		if (read_lps(data, len, &pos, r->auto_id,
			     sizeof(r->auto_id)) < 0) {
			break;
		}

		/* fixed fields: role, op, notify_count, on_timeout,
		 * threshold(f), value(f), timeout_ms(u32) = 16 bytes */
		if (pos + 16 > len) {
			break;
		}
		r->role         = data[pos++];
		r->op           = data[pos++];
		r->notify_count = data[pos++];
		r->on_timeout   = data[pos++];
		memcpy(&r->threshold, data + pos, 4); pos += 4;
		memcpy(&r->value,     data + pos, 4); pos += 4;
		memcpy(&r->timeout_ms, data + pos, 4); pos += 4;

		/* field (lps) */
		if (read_lps(data, len, &pos, r->field,
			     sizeof(r->field)) < 0) {
			break;
		}

		/* notify names (lps × notify_count) */
		int nc = MIN(r->notify_count, AUTO_MAX_NOTIFY);
		for (int j = 0; j < nc; j++) {
			if (read_lps(data, len, &pos, r->notify[j],
				     sizeof(r->notify[j])) < 0) {
				break;
			}
		}
		/* skip extra notify entries beyond AUTO_MAX_NOTIFY */
		for (int j = nc; j < r->notify_count; j++) {
			if (pos >= len) break;
			uint8_t skip = data[pos++];
			pos += skip;
		}
		if (r->notify_count > AUTO_MAX_NOTIFY) {
			r->notify_count = AUTO_MAX_NOTIFY;
		}

		loaded++;
	}

	g_rule_count = loaded;

	NN_LOG_INF("Loaded %d automation rules (%zu bytes)", loaded, pos);
	for (int i = 0; i < g_rule_count; i++) {
		NN_LOG_INF("  [%d] %s role=%c field=%s notify=%d",
			i, g_rules[i].auto_id, g_rules[i].role,
			g_rules[i].field, g_rules[i].notify_count);
	}

	return 0;
}

/* KV load callback: replay the persisted blob into RAM at boot.  Does
 * NOT re-save — that would be a redundant NVS write on every boot. */
static int auto_blob_kv_load(const char *suffix, const uint8_t *value,
			     size_t value_len, void *user)
{
	ARG_UNUSED(user);
	if (strcmp(suffix, "blob") != 0) {
		return 0;   /* not ours */
	}
	int rc = auto_engine_apply_blob(value, value_len);
	if (rc == 0) {
		NN_LOG_INF("auto_engine: restored %d rule(s) from NVS",
			   g_rule_count);
	} else {
		NN_LOG_WRN("auto_engine: NVS blob apply rc=%d (ignored)", rc);
	}
	return 0;
}

int auto_engine_load(const uint8_t *data, size_t len)
{
	int rc = auto_engine_apply_blob(data, len);
	if (rc != 0) {
		return rc;
	}
	/* Persist so the rules survive a reboot / OTA without the hub
	 * having to re-push.  Mirrors how the OT dataset + X25519 keys
	 * already persist.  Best-effort: a save failure leaves the rules
	 * live in RAM for this boot. */
	if (len <= AUTO_BLOB_MAX) {
		int srv = nn_osal_kv_save(AUTO_KV_KEY, data, len);
		if (srv != 0) {
			NN_LOG_WRN("auto_engine: kv_save rc=%d (rules live but "
				   "won't survive reboot)", srv);
		}
	} else {
		NN_LOG_WRN("auto_engine: blob %zu B > %d, not persisted",
			   len, AUTO_BLOB_MAX);
	}
	return 0;
}

int auto_engine_rule_count(void)
{
	return g_rule_count;
}

/* ── Field capability registry ─────────────────────────────────────────────── */

int auto_engine_register_field(const char *name, uint8_t type,
			       float min_val, float max_val)
{
	if (g_field_desc_count >= CONFIG_NODE_MGR_AUTO_MAX_FIELD_DESC) {
		return -ENOMEM;
	}
	struct auto_field_desc *d = &g_field_descs[g_field_desc_count++];
	strncpy(d->name, name, AUTO_MAX_FIELD - 1);
	d->name[AUTO_MAX_FIELD - 1] = '\0';
	d->type = type;
	d->min_val = min_val;
	d->max_val = max_val;
	return 0;
}

const struct auto_field_desc *auto_engine_get_field_descs(int *count)
{
	*count = g_field_desc_count;
	return g_field_descs;
}

int auto_engine_register_actuator_cb(const char *name,
				     auto_engine_actuator_cb cb, void *user)
{
	for (int i = 0; i < g_field_desc_count; i++) {
		if (strncmp(g_field_descs[i].name, name, AUTO_MAX_FIELD) != 0) {
			continue;
		}
		if (g_field_descs[i].type != AUTO_FIELD_TYPE_ACTUATOR) {
			return -ENOTSUP;
		}
		g_actuator_cbs[i].cb   = cb;
		g_actuator_cbs[i].user = user;
		return 0;
	}
	return -ENOENT;
}

/* ── D2D AUTO_NOTIFY receiver ────────────────────────────────────────────
 * Replaces the CoAP /auto handler that lived in coap_info.c.  Each call
 * is a 3× retry from the sender; we dedup by (sender_id, tid) so the
 * action only fires once. */

#define DEDUP_WINDOW 8
struct dedup_entry {
	uint8_t  sender_id[NN_PROTO_CLIENT_DEVICE_ID_LEN];
	size_t   sender_id_len;
	uint32_t tid;
	int64_t  seen_at_ms;
};
static struct dedup_entry s_dedup[DEDUP_WINDOW];
static int                s_dedup_next;

static bool dedup_check(const uint8_t *sender_id, size_t sender_id_len,
			uint32_t tid)
{
	int64_t now = nn_osal_uptime_ms();
	for (int i = 0; i < DEDUP_WINDOW; i++) {
		if (s_dedup[i].sender_id_len == sender_id_len &&
		    s_dedup[i].tid == tid &&
		    memcmp(s_dedup[i].sender_id, sender_id, sender_id_len) == 0 &&
		    (now - s_dedup[i].seen_at_ms) < 5000) {
			return true; /* duplicate */
		}
	}
	int slot = s_dedup_next++ % DEDUP_WINDOW;
	memcpy(s_dedup[slot].sender_id, sender_id,
	       sender_id_len > NN_PROTO_CLIENT_DEVICE_ID_LEN
	       ? NN_PROTO_CLIENT_DEVICE_ID_LEN : sender_id_len);
	s_dedup[slot].sender_id_len = sender_id_len;
	s_dedup[slot].tid = tid;
	s_dedup[slot].seen_at_ms = now;
	return false;
}

static void on_auto_notify_d2d(const uint8_t *sender_id, size_t sender_id_len,
			       const uint8_t sender_ipv6[16],
			       uint32_t tid, const uint8_t *body, size_t body_len,
			       void *user)
{
	ARG_UNUSED(user);
	/* Always ack — even duplicates — so the sender can retire its
	 * slot.  Duplicates only happen because an earlier ACK got lost
	 * (or the sender hasn't seen this one yet); we want to short-
	 * circuit the next retry round. */
	(void)nn_proto_client_send_d2d(sender_ipv6,
				       NN_PROTO_CMD_AUTO_NOTIFY_ACK,
				       tid, NULL, 0);

	if (dedup_check(sender_id, sender_id_len, tid)) {
		NN_LOG_DBG("AUTO_NOTIFY dup tid=%u", tid);
		return;
	}
	if (body_len >= AUTO_BODY_MAX) {
		NN_LOG_WRN("AUTO_NOTIFY body too large: %zu", body_len);
		return;
	}
	char json_copy[AUTO_BODY_MAX];
	memcpy(json_copy, body, body_len);
	json_copy[body_len] = '\0';
	auto_engine_on_message(json_copy);
}

static void on_auto_notify_s_d2d(const uint8_t *sender_id,
				 size_t sender_id_len,
				 const uint8_t sender_ipv6[16],
				 uint32_t tid,
				 const uint8_t *body, size_t body_len,
				 void *user)
{
	ARG_UNUSED(user);
	if (sender_id_len != 8) {
		return;
	}
	/* Ack first (zero-sig — retires the sender's reliable slot even
	 * for duplicates), then authenticate + dedup + dispatch. */
	(void)nn_proto_client_send_d2d_ns(sender_ipv6,
					  NN_PROTO_CMD_AUTO_NOTIFY_ACK,
					  tid, NULL, 0);

	if (dedup_check(sender_id, sender_id_len, tid)) {
		return;
	}
	char json_copy[AUTO_BODY_MAX];
	size_t out_len = 0;
	int rv = nn_group_open(sender_id, NN_PROTO_CMD_AUTO_NOTIFY_S,
			       body, body_len,
			       (uint8_t *)json_copy, sizeof json_copy - 1,
			       &out_len);
	if (rv == -EEXIST) {
		return;                       /* replayed record */
	}
	if (rv != 0) {
		NN_LOG_WRN("AUTO_NOTIFY_S open rv=%d", rv);
		return;
	}
	json_copy[out_len] = '\0';
	auto_engine_on_message(json_copy);
}

/* AUTO_NOTIFY_ACK is consumed automatically by the nn_proto_client
 * reliable layer; no handler registration needed here. */

/* ── Init ─────────────────────────────────────────────────────────────────── */

int auto_engine_init(void)
{
	g_rule_count = 0;
	g_field_count = 0;
	g_tid_counter = 0;
	memset(g_pending, 0, sizeof(g_pending));
	memset(g_my_mleid, 0, sizeof(g_my_mleid));
	memset(s_dedup, 0, sizeof(s_dedup));

	/* Register the D2D inbound handler — replaces coap_info.c's
	 * CoAP /auto receiver.  The AUTO_NOTIFY_ACK return path is
	 * handled automatically inside nn_proto_client when the
	 * sender side called send_d2d_reliable. */
	(void)nn_proto_client_register_d2d_handler(NN_PROTO_CMD_AUTO_NOTIFY,
						   on_auto_notify_d2d, NULL);
	nn_proto_client_register_d2d_handler(NN_PROTO_CMD_AUTO_NOTIFY_S,
					      on_auto_notify_s_d2d, NULL);

	/* Restore persisted automation rules from NVS.  kv_register loads
	 * the "auto" subtree synchronously (settings_load_subtree), so
	 * auto_blob_kv_load runs before this returns — g_rule_count is
	 * populated by the time the app starts handling triggers.  This
	 * closes the post-reboot/OTA gap where the device had zero rules
	 * until the hub re-pushed. */
	(void)nn_osal_kv_register(AUTO_KV_PREFIX, auto_blob_kv_load, NULL);

	/* Cache our ML-EID for self-notification detection.
	 * Note: ML-EID is only valid after Thread attach, so this may be
	 * empty at boot. It gets populated on first trigger if needed. */
	otInstance *inst = (otInstance *)nn_pal_ot_instance();
	if (inst) {
		nn_pal_ot_mutex_lock();
		const otIp6Address *mleid =
			otThreadGetMeshLocalEid(inst);
		if (mleid) {
			char buf[48];
			snprintf(buf, sizeof(buf),
				 "%x:%x:%x:%x:%x:%x:%x:%x",
				 nn_osal_get_be16(&mleid->mFields.m8[0]),
				 nn_osal_get_be16(&mleid->mFields.m8[2]),
				 nn_osal_get_be16(&mleid->mFields.m8[4]),
				 nn_osal_get_be16(&mleid->mFields.m8[6]),
				 nn_osal_get_be16(&mleid->mFields.m8[8]),
				 nn_osal_get_be16(&mleid->mFields.m8[10]),
				 nn_osal_get_be16(&mleid->mFields.m8[12]),
				 nn_osal_get_be16(&mleid->mFields.m8[14]));
			strncpy(g_my_mleid, buf, sizeof(g_my_mleid) - 1);
		}
		nn_pal_ot_mutex_unlock();
	}

	NN_LOG_INF("Auto engine init: mleid=%s (%d rule slots)",
		g_my_mleid[0] ? g_my_mleid : "(not yet)",
		CONFIG_NODE_MGR_AUTO_MAX_RULES);
	return 0;
}
