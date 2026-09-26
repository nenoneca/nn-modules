/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Pure dispatcher for /field ECIES requests, factored out of
 * coap_info.c::handle_field so both the (now-deleted) direct CoAP
 * listener and the Phase-6 nn_proto H2D relay can share one
 * implementation.  No transport-specific code lives here.
 */

#include <nn_osal/osal.h>
#include <node_mgr/coap_field.h>
#include <node_mgr/auto_engine.h>
#include <node_mgr/hub_crypto.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


NN_OSAL_LOG_MODULE(coap_field);

#define PLAIN_BUF_SIZE 512

/* ── tiny JSON value extractors (same shape as coap_info.c) ───────── */

static const char *json_find_str(const char *json, const char *key,
				 size_t *out_len)
{
	char pat[32];
	int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
	if (n < 0 || (size_t)n >= sizeof(pat)) return NULL;
	const char *p = strstr(json, pat);
	if (!p) return NULL;
	p += n;
	while (*p == ' ' || *p == '\t' || *p == ':') p++;
	if (*p != '"') return NULL;
	p++;
	const char *end = strchr(p, '"');
	if (!end) return NULL;
	*out_len = (size_t)(end - p);
	return p;
}

static bool json_find_float(const char *json, const char *key, float *out)
{
	char pat[32];
	int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
	if (n < 0 || (size_t)n >= sizeof(pat)) return false;
	const char *p = strstr(json, pat);
	if (!p) return false;
	p += n;
	while (*p == ' ' || *p == '\t' || *p == ':') p++;
	char *end = NULL;
	float v = strtof(p, &end);
	if (end == p) return false;
	*out = v;
	return true;
}

/* ── field-desc lookup ────────────────────────────────────────────── */

static const struct auto_field_desc *find_field_desc(const char *name)
{
	int fc = 0;
	const struct auto_field_desc *descs = auto_engine_get_field_descs(&fc);
	for (int i = 0; i < fc; i++) {
		if (strncmp(descs[i].name, name, AUTO_MAX_FIELD) == 0) {
			return &descs[i];
		}
	}
	return NULL;
}

/* ── public dispatch ──────────────────────────────────────────────── */

int field_op_dispatch_plain(const char *plain_req,
			    char *resp_out, size_t resp_cap,
			    int *resp_len)
{
	if (!plain_req || !resp_out || !resp_len) {
		return -EINVAL;
	}
	const uint8_t *plain = (const uint8_t *)plain_req;

	/* Parse op + name. */
	size_t op_len = 0, name_len = 0;
	const char *op_p   = json_find_str((char *)plain, "op",   &op_len);
	const char *name_p = json_find_str((char *)plain, "name", &name_len);

	const char *err = NULL;
	char        name[AUTO_MAX_FIELD] = {0};
	float       value = NAN;
	bool        have_value = false;

	if (!op_p || !name_p || name_len == 0 || name_len >= AUTO_MAX_FIELD) {
		err = "bad_request";
	} else {
		memcpy(name, name_p, name_len);
		name[name_len] = '\0';
	}

	/* Virtual-input twin: every read-only physical input `X` is also
	 * addressable as `X_v`, a WRITABLE alias advertised in the field
	 * descriptors.  A SET on the twin injects through the exact same
	 * auto_engine_set_field() path the hardware input uses, so rules
	 * watching the physical field fire identically; a GET mirrors the
	 * base field's live value.  The response echoes the requested
	 * (twin) name so the hub's read-back stays symmetrical. */
	char reqname[AUTO_MAX_FIELD + 2];
	strcpy(reqname, name);
	bool via_twin = false;
	{
		size_t nl = strlen(name);
		if (!err && nl > 2 && strcmp(name + nl - 2, "_v") == 0) {
			name[nl - 2] = '\0';
			via_twin = true;
		}
	}

	const struct auto_field_desc *desc = err ? NULL : find_field_desc(name);
	bool is_get = err ? false : (op_len == 3 && memcmp(op_p, "get", 3) == 0);
	bool is_set = err ? false : (op_len == 3 && memcmp(op_p, "set", 3) == 0);

	if (!err && !is_get && !is_set) err = "unknown_op";
	if (!err && !desc)              err = "unknown_field";
	/* Twins exist only for read-only inputs — actuators are already
	 * writable under their own name, so `led_v` stays unknown. */
	if (!err && via_twin && desc->type != AUTO_FIELD_TYPE_SENSOR)
		err = "unknown_field";

	if (!err && is_set) {
		if (desc->type != AUTO_FIELD_TYPE_ACTUATOR && !via_twin) {
			err = "read_only";
		} else if (!json_find_float((char *)plain, "value", &value)) {
			err = "missing_value";
		} else if (value < desc->min_val || value > desc->max_val) {
			err = "out_of_range";
		} else {
			auto_engine_set_field(name, value);
			have_value = true;
			NN_LOG_INF("SET %s=%.3f", name, (double)value);
		}
	}

	if (!err && is_get) {
		value = auto_engine_get_field(name);
		have_value = !isnan(value);
		NN_LOG_INF("GET %s=%s", name, have_value ? "<value>" : "null");
	}

	/* Build response plaintext. */
	int rp_len;
	if (err) {
		rp_len = snprintf(resp_out, resp_cap,
				  "{\"err\":\"%s\"}", err);
	} else if (have_value) {
		rp_len = snprintf(resp_out, resp_cap,
				  "{\"name\":\"%s\",\"value\":%.3f}",
				  reqname, (double)value);
	} else {
		rp_len = snprintf(resp_out, resp_cap,
				  "{\"name\":\"%s\",\"value\":null}", reqname);
	}
	if (rp_len < 0 || rp_len >= (int)resp_cap) {
		return -ENOMEM;
	}
	*resp_len = rp_len;
	return 0;
}

int field_op_dispatch(const char *env_in,
		      char *env_out, size_t env_out_size,
		      size_t *env_out_len)
{
	if (!env_in || !env_out || !env_out_len) {
		return -EINVAL;
	}
	*env_out_len = 0;

	uint8_t plain[PLAIN_BUF_SIZE];
	size_t  plain_len = sizeof(plain) - 1;

	/* hub_crypto_decrypt mutates env_in in place — give it a writable
	 * copy.  Stack-budget OK: env_in is small (~300 B). */
	char in_copy[768];
	size_t in_len = strnlen(env_in, sizeof(in_copy) - 1);
	memcpy(in_copy, env_in, in_len);
	in_copy[in_len] = '\0';

	int rc = hub_crypto_decrypt(in_copy, plain, &plain_len);
	if (rc != 0) {
		NN_LOG_WRN("decrypt failed: %d", rc);
		return -EINVAL;
	}
	plain[plain_len] = '\0';

	char resp_plain[PLAIN_BUF_SIZE];
	int  rp_len = 0;
	rc = field_op_dispatch_plain((const char *)plain,
				     resp_plain, sizeof(resp_plain), &rp_len);
	if (rc != 0) {
		return rc;
	}

	rc = hub_crypto_encrypt((const uint8_t *)resp_plain, (size_t)rp_len,
				env_out, env_out_size);
	if (rc != 0) {
		NN_LOG_ERR("encrypt failed: %d", rc);
		return -ENOMEM;
	}
	*env_out_len = strnlen(env_out, env_out_size);
	return 0;
}
