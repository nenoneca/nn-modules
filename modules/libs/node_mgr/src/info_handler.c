/* SPDX-License-Identifier: Apache-2.0 */

/*
 * info_handler.c — H2D INFO_QUERY responder + device-initiated config sync.
 *
 * The hub uses INFO_REPLY (cmd 0x0025) for two flows:
 *
 *   1. Paired response: hub sends an H2D INFO_QUERY with a tid; we
 *      reply with an INFO_REPLY carrying the same tid.
 *
 *   2. Unsolicited (tid=0): we push our config to the hub on our own
 *      initiative — at boot after Thread attach, and on state changes
 *      (e.g. after an AUTO_PUSH applies a new rule blob).
 *
 * Both use the same JSON body shape, so the hub has one parser.
 * Wire: payload is a signed nn_proto frame; INFO_REPLY body is the
 * JSON string (no length prefix; frame length is authoritative).
 */

#include <nn_osal/osal.h>
#include <node_mgr/auto_engine.h>
#include <node_mgr/info_handler.h>
#include <node_mgr/nn_proto_client.h>
#include <node_mgr/ota_client.h>   /* armed_version for the config sync */
#include <node_mgr/nn_session_boot.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <nn_pal/openthread.h>

#include <openthread/instance.h>
#include <openthread/link.h>

#include <nn_proto/nn_proto.h>

NN_OSAL_LOG_MODULE(info_handler);

#define INFO_RESP_MAX  768

static const char *g_device_name;
static const char *g_image_name;   /* build-time app identity, e.g. "mdns_ot_esp32c6" */

/* Build the device-info JSON into `out` (capacity `out_max`).
 * Returns the number of bytes written (excluding NUL), or negative
 * errno on overflow / formatting error. */
static int build_info_json(char *out, size_t out_max)
{
	const char *name    = g_device_name ? g_device_name : "(unset)";
	const char *fw      = nn_osal_app_version() ? nn_osal_app_version() : "";

	char eui64_str[17] = "0000000000000000";
	otInstance *inst = (otInstance *)nn_pal_ot_instance();
	if (inst) {
		nn_pal_ot_mutex_lock();
		const otExtAddress *eui = otLinkGetExtendedAddress(inst);
		if (eui) {
			snprintf(eui64_str, sizeof eui64_str,
				 "%02x%02x%02x%02x%02x%02x%02x%02x",
				 eui->m8[0], eui->m8[1], eui->m8[2], eui->m8[3],
				 eui->m8[4], eui->m8[5], eui->m8[6], eui->m8[7]);
		}
		nn_pal_ot_mutex_unlock();
	}

	/* Every registered descriptor, PLUS a writable virtual twin
	 * (`<name>_v`, t=1) for each read-only physical input — writes to
	 * the twin inject through auto_engine_set_field(<name>) on the
	 * device (coap_field.c), so hub-side rules watching the physical
	 * field fire identically.  Sized for ~6 real + ~4 twin entries;
	 * snprintf caps silently, so keep headroom (256 once truncated the
	 * list when a 7th field was added). */
	char fields_json[640] = "[]";
	int fc = 0;
	const struct auto_field_desc *descs = auto_engine_get_field_descs(&fc);
	if (fc > 0) {
		int pos = 0;
		bool first = true;
		pos += snprintf(fields_json + pos, sizeof fields_json - pos, "[");
		for (int i = 0; i < fc; i++) {
			if (!first) {
				pos += snprintf(fields_json + pos,
						sizeof fields_json - pos, ",");
			}
			first = false;
			pos += snprintf(fields_json + pos,
					sizeof fields_json - pos,
					"{\"n\":\"%s\",\"t\":%d,"
					"\"min\":%.0f,\"max\":%.0f}",
					descs[i].name, descs[i].type,
					(double)descs[i].min_val,
					(double)descs[i].max_val);
			if (descs[i].type == AUTO_FIELD_TYPE_SENSOR) {
				pos += snprintf(fields_json + pos,
						sizeof fields_json - pos,
						",{\"n\":\"%s_v\",\"t\":1,"
						"\"min\":%.0f,\"max\":%.0f}",
						descs[i].name,
						(double)descs[i].min_val,
						(double)descs[i].max_val);
			}
		}
		snprintf(fields_json + pos, sizeof fields_json - pos, "]");
	}

	char sess_hex[17] = "";
	(void)nn_session_boot_salt_hex(sess_hex);

	int rlen = snprintf(out, out_max,
			    "{\"firmware\":\"%s\","
			    "\"name\":\"%s\","
			    "\"img\":\"%s\","
			    "\"eui64\":\"%s\","
			    "\"uptime_ms\":%u,"
			    "\"armed\":\"%s\","
			    "\"sess\":\"%s\","
			    "\"fields\":%s}",
			    fw, name,
			    g_image_name ? g_image_name : "",
			    eui64_str,
			    (unsigned)nn_osal_uptime_ms_32(),
			    ota_client_armed_version(),
			    sess_hex,
			    fields_json);
	if (rlen <= 0 || (size_t)rlen >= out_max) {
		NN_LOG_ERR("info: response too long (%d / %u)", rlen,
			   (unsigned)out_max);
		return -ENOSPC;
	}
	return rlen;
}

static void on_info_query(uint32_t tid,
			  const uint8_t *body, size_t body_len,
			  void *user)
{
	ARG_UNUSED(body); ARG_UNUSED(body_len); ARG_UNUSED(user);

	char resp[INFO_RESP_MAX];
	int rlen = build_info_json(resp, sizeof resp);
	if (rlen < 0) {
		return;
	}

	int rv = nn_proto_client_send_d2h_reply(NN_PROTO_CMD_INFO_REPLY, tid,
						(const uint8_t *)resp,
						(size_t)rlen);
	NN_LOG_INF("INFO_REPLY tid=0x%08x (%d B) rv=%d", tid, rlen, rv);
}

int info_handler_send_unsolicited(void)
{
	char resp[INFO_RESP_MAX];
	int rlen = build_info_json(resp, sizeof resp);
	if (rlen < 0) {
		return rlen;
	}
	/* tid=0 marks this as an unsolicited config-sync push. */
	int rv = nn_proto_client_send_d2h_reply(NN_PROTO_CMD_INFO_REPLY, 0,
						(const uint8_t *)resp,
						(size_t)rlen);
	NN_LOG_INF("INFO_REPLY unsolicited (%d B) rv=%d", rlen, rv);
	return rv;
}

int info_handler_start(const char *device_name, const char *image_name)
{
	g_device_name = device_name;
	g_image_name = image_name;
	return nn_proto_client_register_h2d_handler(NN_PROTO_CMD_INFO_QUERY,
						    on_info_query, NULL);
}
