/* SPDX-License-Identifier: Apache-2.0 */

/*
 * BlueZ D-Bus backend for fw_common/gw_ble_prov.  Implements a small
 * sd-bus mainloop that registers a GATT application + LE advertisement
 * with bluetoothd, then routes ReadValue/WriteValue/StartNotify into
 * the platform-neutral protocol module.
 *
 * BlueZ-facing API surface used here:
 *   org.bluez.GattManager1.RegisterApplication           — /org/bluez/hci0
 *   org.bluez.LEAdvertisingManager1.RegisterAdvertisement
 *   org.bluez.GattService1                               — props: UUID, Primary
 *   org.bluez.GattCharacteristic1                        — props: UUID, Service,
 *                                                          Flags, Value
 *                                                          methods: ReadValue,
 *                                                          WriteValue, StartNotify,
 *                                                          StopNotify
 *   org.bluez.LEAdvertisement1                           — props: Type,
 *                                                          ServiceUUIDs,
 *                                                          LocalName
 *                                                          methods: Release
 *   org.freedesktop.DBus.ObjectManager  (auto-emitted via
 *                                       sd_bus_add_object_manager)
 *
 * Threading: a single mainloop in gw_ble_prov_linux_run() pumps the bus
 * and a pthread runs decrypt work off the bus thread.  Both call into
 * gw_ble_prov via the protocol API; serialization is via g_proto_mu.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#include <fw_common/gw_ble_prov.h>
#include <fw_common/gw_ble_prov_linux.h>
#include <fw_common/log.h>

LOG_MODULE_REGISTER(gw_ble_prov_be_linux, LOG_LEVEL_INF);

/* ── paths + names ────────────────────────────────────────────────── */

#define APP_PATH      "/com/nn/gw_prov"
#define SVC_PATH      APP_PATH "/service0"
#define ADV_PATH      APP_PATH "/adv0"

/* Characteristic index — order is fixed and used to compute the D-Bus
 * object path "service0/charN". */
enum char_idx {
	CHAR_WIFI_CRED = 0,
	CHAR_HUB_HOST,
	CHAR_HUB_ID,
	CHAR_INFO,
	CHAR_STATUS,
	CHAR_COMMIT,
	CHAR_HUB_X25519,
	CHAR_OT_DATASET,
	N_CHARS,
};

/* BlueZ characteristic flags (space-separated array of strings).  We use:
 *   "write" + "write-without-response"  for the encrypted-payload writes
 *   "write"                              for HUB_X25519 + COMMIT
 *   "read"                               for INFO
 *   "read" + "notify"                    for STATUS
 */
struct char_descr {
	enum char_idx idx;
	const char *uuid;
	const char *flags[4];  /* NULL-terminated */
};

static const struct char_descr CHARS[N_CHARS] = {
	{ CHAR_WIFI_CRED,  GW_BLE_PROV_WIFI_CRED_UUID_STR,
	  { "write", "write-without-response", NULL } },
	{ CHAR_HUB_HOST,   GW_BLE_PROV_HUB_HOST_UUID_STR,
	  { "write", "write-without-response", NULL } },
	{ CHAR_HUB_ID,     GW_BLE_PROV_HUB_IDENTITY_UUID_STR,
	  { "write", "write-without-response", NULL } },
	{ CHAR_INFO,       GW_BLE_PROV_INFO_UUID_STR,
	  { "read", NULL } },
	{ CHAR_STATUS,     GW_BLE_PROV_STATUS_UUID_STR,
	  { "read", "notify", NULL } },
	{ CHAR_COMMIT,     GW_BLE_PROV_COMMIT_UUID_STR,
	  { "write", NULL } },
	{ CHAR_HUB_X25519, GW_BLE_PROV_HUB_X25519_UUID_STR,
	  { "write", NULL } },
	{ CHAR_OT_DATASET, GW_BLE_PROV_OT_DATASET_UUID_STR,
	  { "write", "write-without-response", NULL } },
};

static char g_adapter[128] = "/org/bluez/hci0";

/* ── runtime state ────────────────────────────────────────────────── */

static sd_bus *g_bus;
static sd_bus_slot *g_svc_slot;
static sd_bus_slot *g_char_slots[N_CHARS];
static sd_bus_slot *g_adv_slot;
static volatile sig_atomic_t g_running = 1;

/* Mutex serializing protocol-module access (bus thread vs. decrypt thread). */
static pthread_mutex_t g_proto_mu = PTHREAD_MUTEX_INITIALIZER;

/* STATUS notify subscription tracking + last value (for PropertiesChanged). */
static bool          g_status_notifying;
static uint8_t       g_status_last;

/* Single-slot decrypt work — backend serializes one decrypt at a time
 * via the protocol's own s_pending.busy flag; we still need to wake a
 * pthread to actually run gw_ble_prov_do_decrypt(). */
static pthread_t       g_decrypt_thread;
static pthread_mutex_t g_decrypt_mu  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_decrypt_cv  = PTHREAD_COND_INITIALIZER;
static bool            g_decrypt_pending;
static bool            g_apply_pending;
static bool            g_decrypt_thread_running;

/* Reboot scheduling (1 s delayed exit(0)).  Done on the bus thread via
 * a one-shot timer event source. */
static sd_event_source *g_reboot_timer;
static sd_event        *g_event;

/* ── helpers ──────────────────────────────────────────────────────── */

static int compose_char_path(char *out, size_t cap, enum char_idx i)
{
	return snprintf(out, cap, SVC_PATH "/char%u", (unsigned)i);
}

static int append_string_array(sd_bus_message *m, const char *const *arr)
{
	int r = sd_bus_message_open_container(m, 'a', "s");
	if (r < 0) return r;
	for (size_t i = 0; arr[i]; i++) {
		r = sd_bus_message_append(m, "s", arr[i]);
		if (r < 0) return r;
	}
	return sd_bus_message_close_container(m);
}

/* Read the BlueZ adapter's "Address" property and turn it into the
 * 6-byte MAC we feed to gw_ble_prov_init. */
static int read_adapter_mac(uint8_t mac6[6])
{
	memset(mac6, 0, 6);
	sd_bus_error err = SD_BUS_ERROR_NULL;
	char *addr = NULL;
	int r = sd_bus_get_property_string(
		g_bus, "org.bluez", g_adapter, "org.bluez.Adapter1",
		"Address", &err, &addr);
	if (r < 0) {
		LOG_WRN("Adapter1.Address: %s", err.message ?: strerror(-r));
		sd_bus_error_free(&err);
		return r;
	}
	unsigned bytes[6];
	if (sscanf(addr, "%x:%x:%x:%x:%x:%x",
		   &bytes[0], &bytes[1], &bytes[2],
		   &bytes[3], &bytes[4], &bytes[5]) != 6) {
		LOG_WRN("can't parse adapter MAC '%s'", addr);
		free(addr);
		return -EPROTO;
	}
	for (int i = 0; i < 6; i++) mac6[i] = (uint8_t)bytes[i];
	free(addr);
	return 0;
}

/* ── GattService1 vtable ──────────────────────────────────────────── */

static int svc_get_uuid(sd_bus *bus, const char *path, const char *iface,
			const char *prop, sd_bus_message *r,
			void *u, sd_bus_error *e)
{
	(void)bus; (void)path; (void)iface; (void)prop; (void)u; (void)e;
	return sd_bus_message_append(r, "s", GW_BLE_PROV_SVC_UUID_STR);
}

static int svc_get_primary(sd_bus *bus, const char *path, const char *iface,
			   const char *prop, sd_bus_message *r,
			   void *u, sd_bus_error *e)
{
	(void)bus; (void)path; (void)iface; (void)prop; (void)u; (void)e;
	return sd_bus_message_append(r, "b", 1);
}

static const sd_bus_vtable svc_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("UUID",    "s", svc_get_uuid,    0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Primary", "b", svc_get_primary, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

/* ── GattCharacteristic1 vtable ───────────────────────────────────── */

static const struct char_descr *char_descr_for(const char *path)
{
	for (size_t i = 0; i < N_CHARS; i++) {
		char p[96];
		compose_char_path(p, sizeof p, (enum char_idx)i);
		if (!strcmp(p, path)) return &CHARS[i];
	}
	return NULL;
}

static int char_get_uuid(sd_bus *bus, const char *path, const char *iface,
			 const char *prop, sd_bus_message *r,
			 void *u, sd_bus_error *e)
{
	(void)bus; (void)iface; (void)prop; (void)u; (void)e;
	const struct char_descr *d = char_descr_for(path);
	if (!d) return sd_bus_error_set(e, SD_BUS_ERROR_UNKNOWN_OBJECT, path);
	return sd_bus_message_append(r, "s", d->uuid);
}

static int char_get_service(sd_bus *bus, const char *path, const char *iface,
			    const char *prop, sd_bus_message *r,
			    void *u, sd_bus_error *e)
{
	(void)bus; (void)path; (void)iface; (void)prop; (void)u; (void)e;
	return sd_bus_message_append(r, "o", SVC_PATH);
}

static int char_get_flags(sd_bus *bus, const char *path, const char *iface,
			  const char *prop, sd_bus_message *r,
			  void *u, sd_bus_error *e)
{
	(void)bus; (void)iface; (void)prop; (void)u;
	const struct char_descr *d = char_descr_for(path);
	if (!d) return sd_bus_error_set(e, SD_BUS_ERROR_UNKNOWN_OBJECT, path);
	return append_string_array(r, d->flags);
}

static int char_get_value(sd_bus *bus, const char *path, const char *iface,
			  const char *prop, sd_bus_message *r,
			  void *u, sd_bus_error *e)
{
	(void)bus; (void)iface; (void)prop; (void)u;
	const struct char_descr *d = char_descr_for(path);
	if (!d) return sd_bus_error_set(e, SD_BUS_ERROR_UNKNOWN_OBJECT, path);
	uint8_t buf[GW_BLE_PROV_INFO_LEN];
	size_t n = 0;
	pthread_mutex_lock(&g_proto_mu);
	if (d->idx == CHAR_INFO) {
		n = gw_ble_prov_read_gw_info(buf, sizeof buf);
	} else if (d->idx == CHAR_STATUS) {
		n = gw_ble_prov_read_status(buf, sizeof buf);
	}
	pthread_mutex_unlock(&g_proto_mu);
	return sd_bus_message_append_array(r, 'y', buf, n);
}

static int char_read_value(sd_bus_message *m, void *user, sd_bus_error *e)
{
	(void)user;
	const char *path = sd_bus_message_get_path(m);
	const struct char_descr *d = char_descr_for(path);
	if (!d) return sd_bus_reply_method_errorf(
			m, SD_BUS_ERROR_UNKNOWN_OBJECT, "%s", path);

	uint8_t buf[GW_BLE_PROV_INFO_LEN];
	size_t n = 0;
	pthread_mutex_lock(&g_proto_mu);
	if (d->idx == CHAR_INFO) {
		n = gw_ble_prov_read_gw_info(buf, sizeof buf);
	} else if (d->idx == CHAR_STATUS) {
		n = gw_ble_prov_read_status(buf, sizeof buf);
	}
	pthread_mutex_unlock(&g_proto_mu);

	sd_bus_message *r = NULL;
	int rc = sd_bus_message_new_method_return(m, &r);
	if (rc < 0) return rc;
	rc = sd_bus_message_append_array(r, 'y', buf, n);
	if (rc < 0) { sd_bus_message_unref(r); return rc; }
	rc = sd_bus_send(NULL, r, NULL);
	sd_bus_message_unref(r);
	return rc < 0 ? rc : 1;
}

static int dispatch_write(const struct char_descr *d,
			  const uint8_t *buf, size_t len)
{
	int rc = 0;
	pthread_mutex_lock(&g_proto_mu);
	switch (d->idx) {
	case CHAR_WIFI_CRED:  rc = gw_ble_prov_write_wifi_cred (buf, len); break;
	case CHAR_HUB_HOST:   rc = gw_ble_prov_write_hub_host  (buf, len); break;
	case CHAR_HUB_ID:     rc = gw_ble_prov_write_hub_id    (buf, len); break;
	case CHAR_OT_DATASET: rc = gw_ble_prov_write_ot_dataset(buf, len); break;
	case CHAR_HUB_X25519: rc = gw_ble_prov_write_hub_x25519(buf, len); break;
	case CHAR_COMMIT:     rc = gw_ble_prov_write_commit    (buf, len); break;
	default:              rc = -ENOSYS; break;
	}
	pthread_mutex_unlock(&g_proto_mu);
	return rc;
}

static int char_write_value(sd_bus_message *m, void *user, sd_bus_error *e)
{
	(void)user;
	const char *path = sd_bus_message_get_path(m);
	const struct char_descr *d = char_descr_for(path);
	if (!d) return sd_bus_reply_method_errorf(
			m, SD_BUS_ERROR_UNKNOWN_OBJECT, "%s", path);

	const void *raw = NULL;
	size_t len = 0;
	int rc = sd_bus_message_read_array(m, 'y', &raw, &len);
	if (rc < 0) return rc;
	/* Skip options dict a{sv}. */
	rc = sd_bus_message_skip(m, "a{sv}");
	if (rc < 0) return rc;

	int wrv = dispatch_write(d, raw, len);
	if (wrv < 0) {
		const char *iface_err = (wrv == -EBUSY || wrv == -EFBIG)
			? "org.bluez.Error.NotPermitted"
			: "org.bluez.Error.InvalidValueLength";
		return sd_bus_reply_method_errorf(m, iface_err,
						  "write rv=%d", wrv);
	}
	return sd_bus_reply_method_return(m, NULL);
}

static int char_start_notify(sd_bus_message *m, void *user, sd_bus_error *e)
{
	(void)user; (void)e;
	const char *path = sd_bus_message_get_path(m);
	const struct char_descr *d = char_descr_for(path);
	if (!d || d->idx != CHAR_STATUS) {
		return sd_bus_reply_method_errorf(
			m, "org.bluez.Error.NotSupported", "no notify here");
	}
	g_status_notifying = true;
	LOG_INF("STATUS notifications ENABLED");
	return sd_bus_reply_method_return(m, NULL);
}

static int char_stop_notify(sd_bus_message *m, void *user, sd_bus_error *e)
{
	(void)user; (void)e;
	const char *path = sd_bus_message_get_path(m);
	const struct char_descr *d = char_descr_for(path);
	if (!d || d->idx != CHAR_STATUS) {
		return sd_bus_reply_method_return(m, NULL);
	}
	g_status_notifying = false;
	LOG_INF("STATUS notifications DISABLED");
	return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable char_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("UUID",    "s",  char_get_uuid,    0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Service", "o",  char_get_service, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Flags",   "as", char_get_flags,   0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Value",   "ay", char_get_value,   0,
			SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_METHOD_WITH_ARGS("ReadValue",
		SD_BUS_ARGS("a{sv}", options),
		SD_BUS_RESULT("ay", value),
		char_read_value, 0),
	SD_BUS_METHOD_WITH_ARGS("WriteValue",
		SD_BUS_ARGS("ay", value, "a{sv}", options),
		SD_BUS_NO_RESULT,
		char_write_value, 0),
	SD_BUS_METHOD("StartNotify", NULL, NULL, char_start_notify, 0),
	SD_BUS_METHOD("StopNotify",  NULL, NULL, char_stop_notify,  0),
	SD_BUS_VTABLE_END
};

/* ── LEAdvertisement1 vtable ──────────────────────────────────────── */

static int adv_get_type(sd_bus *bus, const char *path, const char *iface,
			const char *prop, sd_bus_message *r,
			void *u, sd_bus_error *e)
{
	(void)bus; (void)path; (void)iface; (void)prop; (void)u; (void)e;
	return sd_bus_message_append(r, "s", "peripheral");
}

static int adv_get_service_uuids(sd_bus *bus, const char *path,
				 const char *iface, const char *prop,
				 sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)bus; (void)path; (void)iface; (void)prop; (void)u; (void)e;
	const char *arr[] = { GW_BLE_PROV_SVC_UUID_STR, NULL };
	return append_string_array(r, arr);
}

static int adv_get_local_name(sd_bus *bus, const char *path,
			      const char *iface, const char *prop,
			      sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)bus; (void)path; (void)iface; (void)prop; (void)u; (void)e;
	return sd_bus_message_append(r, "s", gw_ble_prov_get_name());
}

/* BlueZ leaves Min/MaxInterval at 0 if we don't supply them, and on
 * bluez 5.82 + BCM4345 that means the controller never gets enabled —
 * Add Extended Advertising Parameters succeeds but no Set Extended
 * Advertising Enable follows.  Set explicit intervals matching the
 * Zephyr peripheral's fast-adv profile (≈100–200 ms). */
static int adv_get_min_interval(sd_bus *bus, const char *path,
				const char *iface, const char *prop,
				sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)bus; (void)path; (void)iface; (void)prop; (void)u; (void)e;
	return sd_bus_message_append(r, "u", (uint32_t)100);
}

static int adv_get_max_interval(sd_bus *bus, const char *path,
				const char *iface, const char *prop,
				sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)bus; (void)path; (void)iface; (void)prop; (void)u; (void)e;
	return sd_bus_message_append(r, "u", (uint32_t)200);
}

static int adv_get_discoverable(sd_bus *bus, const char *path,
				const char *iface, const char *prop,
				sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)bus; (void)path; (void)iface; (void)prop; (void)u; (void)e;
	return sd_bus_message_append(r, "b", 1);
}

static int adv_release(sd_bus_message *m, void *u, sd_bus_error *e)
{
	(void)u; (void)e;
	LOG_INF("Advertisement.Release called by BlueZ");
	return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable adv_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("Type",         "s",  adv_get_type,          0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("ServiceUUIDs", "as", adv_get_service_uuids, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("LocalName",    "s",  adv_get_local_name,    0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("MinInterval",  "u",  adv_get_min_interval,  0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("MaxInterval",  "u",  adv_get_max_interval,  0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Discoverable", "b",  adv_get_discoverable,  0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_METHOD("Release", NULL, NULL, adv_release, 0),
	SD_BUS_VTABLE_END
};

/* ── BlueZ registration calls ─────────────────────────────────────── */

/*
 * BlueZ's RegisterApplication / RegisterAdvertisement triggers a
 * callback INTO our daemon (e.g. ObjectManager.GetManagedObjects) before
 * it replies.  Using sd_bus_call() here deadlocks: that synchronous
 * helper only dispatches incoming REPLIES while waiting, not method
 * calls, so BlueZ's callback never reaches our vtables and we never get
 * the reply.  We must issue these calls asynchronously through the
 * attached event loop so the loop can serve BlueZ's GetManagedObjects
 * call concurrently with the in-flight reply wait.
 */

static int register_app_reply(sd_bus_message *m, void *user, sd_bus_error *e)
{
	(void)user; (void)e;
	const sd_bus_error *err = sd_bus_message_get_error(m);
	if (err && err->message) {
		LOG_ERR("RegisterApplication reply: %s: %s",
			err->name ?: "(no name)", err->message);
		return 0;
	}
	LOG_INF("GATT app registered at %s", APP_PATH);
	return 0;
}

static int register_adv_reply(sd_bus_message *m, void *user, sd_bus_error *e)
{
	(void)user; (void)e;
	const sd_bus_error *err = sd_bus_message_get_error(m);
	if (err && err->message) {
		LOG_ERR("RegisterAdvertisement reply: %s: %s",
			err->name ?: "(no name)", err->message);
		return 0;
	}
	LOG_INF("LE advertisement registered at %s (name='%s')",
		ADV_PATH, gw_ble_prov_get_name());
	return 0;
}

static int register_application_async(void)
{
	int rc = sd_bus_call_method_async(
		g_bus, NULL, "org.bluez", g_adapter,
		"org.bluez.GattManager1", "RegisterApplication",
		register_app_reply, NULL,
		"oa{sv}", APP_PATH, 0);
	if (rc < 0) {
		LOG_ERR("RegisterApplication async send: %s", strerror(-rc));
		return rc;
	}
	return 0;
}

static int register_advertisement_async(void)
{
	int rc = sd_bus_call_method_async(
		g_bus, NULL, "org.bluez", g_adapter,
		"org.bluez.LEAdvertisingManager1", "RegisterAdvertisement",
		register_adv_reply, NULL,
		"oa{sv}", ADV_PATH, 0);
	if (rc < 0) {
		LOG_ERR("RegisterAdvertisement async send: %s", strerror(-rc));
		return rc;
	}
	return 0;
}

static void unregister_both(void)
{
	if (!g_bus) return;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_call_method(g_bus, "org.bluez", g_adapter,
			   "org.bluez.LEAdvertisingManager1",
			   "UnregisterAdvertisement",
			   &err, NULL, "o", ADV_PATH);
	sd_bus_error_free(&err);
	sd_bus_call_method(g_bus, "org.bluez", g_adapter,
			   "org.bluez.GattManager1",
			   "UnregisterApplication",
			   &err, NULL, "o", APP_PATH);
	sd_bus_error_free(&err);
}

/* ── backend ops → protocol ───────────────────────────────────────── */

static void *decrypt_thread_main(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&g_decrypt_mu);
	while (g_decrypt_thread_running) {
		while (g_decrypt_thread_running &&
		       !g_decrypt_pending && !g_apply_pending) {
			pthread_cond_wait(&g_decrypt_cv, &g_decrypt_mu);
		}
		bool do_dec   = g_decrypt_pending;
		bool do_apply = g_apply_pending;
		g_decrypt_pending = false;
		g_apply_pending   = false;
		pthread_mutex_unlock(&g_decrypt_mu);

		if (do_dec) {
			pthread_mutex_lock(&g_proto_mu);
			gw_ble_prov_do_decrypt();
			pthread_mutex_unlock(&g_proto_mu);
		}
		if (do_apply) {
			pthread_mutex_lock(&g_proto_mu);
			gw_ble_prov_do_apply();
			pthread_mutex_unlock(&g_proto_mu);
		}
		pthread_mutex_lock(&g_decrypt_mu);
	}
	pthread_mutex_unlock(&g_decrypt_mu);
	return NULL;
}

static void be_notify_status(uint8_t status, void *user)
{
	(void)user;
	g_status_last = status;
	if (!g_bus || !g_status_notifying) return;
	char path[96];
	compose_char_path(path, sizeof path, CHAR_STATUS);
	const char *changed[] = { "Value", NULL };
	(void)sd_bus_emit_properties_changed_strv(
		g_bus, path, "org.bluez.GattCharacteristic1",
		(char **)changed);
}

static void be_schedule_decrypt(void *user)
{
	(void)user;
	pthread_mutex_lock(&g_decrypt_mu);
	g_decrypt_pending = true;
	pthread_cond_signal(&g_decrypt_cv);
	pthread_mutex_unlock(&g_decrypt_mu);
}

static void be_schedule_apply(void *user)
{
	(void)user;
	pthread_mutex_lock(&g_decrypt_mu);
	g_apply_pending = true;
	pthread_cond_signal(&g_decrypt_cv);
	pthread_mutex_unlock(&g_decrypt_mu);
}

static int reboot_timer_cb(sd_event_source *s, uint64_t usec, void *ud)
{
	(void)s; (void)usec; (void)ud;
	LOG_INF("provisioning success — exiting (let supervisor restart us)");
	_exit(0);
}

static void be_schedule_reboot_1s(void *user)
{
	(void)user;
	if (!g_event) {
		/* No mainloop attached → drop the delay and exit directly.
		 * Caller is responsible for ordering. */
		LOG_WRN("no sd-event; exit(0) immediately");
		_exit(0);
	}
	uint64_t now;
	sd_event_now(g_event, CLOCK_MONOTONIC, &now);
	sd_event_add_time(g_event, &g_reboot_timer, CLOCK_MONOTONIC,
			  now + 1000000ULL, 0, reboot_timer_cb, NULL);
}

/* ── lifecycle ────────────────────────────────────────────────────── */

static const struct gw_ble_prov_backend g_be = {
	.notify_status      = be_notify_status,
	.schedule_decrypt   = be_schedule_decrypt,
	.schedule_apply     = be_schedule_apply,
	.schedule_reboot_1s = be_schedule_reboot_1s,
	.user               = NULL,
};

static void sig_handler(int signo)
{
	(void)signo;
	g_running = 0;
	if (g_event) sd_event_exit(g_event, 0);
}

int gw_ble_prov_linux_start(const char *adapter)
{
	if (adapter && adapter[0]) {
		snprintf(g_adapter, sizeof g_adapter, "%s", adapter);
	}

	int rc = sd_bus_open_system(&g_bus);
	if (rc < 0) {
		LOG_ERR("sd_bus_open_system: %s", strerror(-rc));
		return rc;
	}

	uint8_t mac[6];
	if (read_adapter_mac(mac) < 0) {
		LOG_WRN("falling back to zero MAC for adv name");
	}
	rc = gw_ble_prov_init(&g_be, mac);
	if (rc < 0) {
		LOG_ERR("gw_ble_prov_init: %d", rc);
		return rc;
	}

	rc = sd_bus_add_object_manager(g_bus, NULL, APP_PATH);
	if (rc < 0) {
		LOG_ERR("add_object_manager: %s", strerror(-rc));
		return rc;
	}

	rc = sd_bus_add_object_vtable(g_bus, &g_svc_slot,
				      SVC_PATH, "org.bluez.GattService1",
				      svc_vtable, NULL);
	if (rc < 0) {
		LOG_ERR("add svc vtable: %s", strerror(-rc));
		return rc;
	}

	for (size_t i = 0; i < N_CHARS; i++) {
		char p[96];
		compose_char_path(p, sizeof p, (enum char_idx)i);
		rc = sd_bus_add_object_vtable(g_bus, &g_char_slots[i],
					      p, "org.bluez.GattCharacteristic1",
					      char_vtable, NULL);
		if (rc < 0) {
			LOG_ERR("add char[%zu] vtable: %s", i, strerror(-rc));
			return rc;
		}
	}

	rc = sd_bus_add_object_vtable(g_bus, &g_adv_slot,
				      ADV_PATH, "org.bluez.LEAdvertisement1",
				      adv_vtable, NULL);
	if (rc < 0) {
		LOG_ERR("add adv vtable: %s", strerror(-rc));
		return rc;
	}

	/* The actual RegisterApplication / RegisterAdvertisement calls run
	 * asynchronously inside the mainloop — see gw_ble_prov_linux_run().
	 * They can't be issued here: doing so triggers a BlueZ → our daemon
	 * GetManagedObjects callback which sd_bus_call cannot dispatch
	 * while it blocks waiting for the RegisterApplication reply (sd_bus_call
	 * dispatches only REPLIES, not method calls, so it deadlocks). */

	g_decrypt_thread_running = true;
	if (pthread_create(&g_decrypt_thread, NULL,
			   decrypt_thread_main, NULL) != 0) {
		LOG_ERR("pthread_create(decrypt): %s", strerror(errno));
		g_decrypt_thread_running = false;
		unregister_both();
		return -errno;
	}

	signal(SIGINT,  sig_handler);
	signal(SIGTERM, sig_handler);
	return 0;
}

int gw_ble_prov_linux_run(void)
{
	int rc = sd_event_default(&g_event);
	if (rc < 0) {
		LOG_ERR("sd_event_default: %s", strerror(-rc));
		return rc;
	}
	rc = sd_bus_attach_event(g_bus, g_event, SD_EVENT_PRIORITY_NORMAL);
	if (rc < 0) {
		LOG_ERR("sd_bus_attach_event: %s", strerror(-rc));
		return rc;
	}

	/* Fire RegisterApplication + RegisterAdvertisement now that the
	 * event loop is dispatching incoming method calls (BlueZ will
	 * call ObjectManager.GetManagedObjects on us as part of the
	 * registration handshake). */
	rc = register_application_async();
	if (rc < 0) return rc;
	rc = register_advertisement_async();
	if (rc < 0) return rc;

	LOG_INF("entering mainloop (SIGINT/SIGTERM to stop)");
	rc = sd_event_loop(g_event);
	LOG_INF("mainloop exit rc=%d", rc);
	return rc < 0 ? rc : 0;
}

void gw_ble_prov_linux_stop(void)
{
	g_running = 0;
	unregister_both();

	pthread_mutex_lock(&g_decrypt_mu);
	g_decrypt_thread_running = false;
	pthread_cond_signal(&g_decrypt_cv);
	pthread_mutex_unlock(&g_decrypt_mu);
	if (g_decrypt_thread) pthread_join(g_decrypt_thread, NULL);

	if (g_adv_slot) { sd_bus_slot_unref(g_adv_slot); g_adv_slot = NULL; }
	if (g_svc_slot) { sd_bus_slot_unref(g_svc_slot); g_svc_slot = NULL; }
	for (size_t i = 0; i < N_CHARS; i++) {
		if (g_char_slots[i]) {
			sd_bus_slot_unref(g_char_slots[i]);
			g_char_slots[i] = NULL;
		}
	}
	if (g_event) { sd_event_unref(g_event); g_event = NULL; }
	if (g_bus)   { sd_bus_unref(g_bus);     g_bus = NULL; }
}
