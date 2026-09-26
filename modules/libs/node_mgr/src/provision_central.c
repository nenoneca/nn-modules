/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Sensor-as-central BLE provisioning helper.  Migrated to nn_pal/ble.h:
 * scan / connect / discover / read / write / subscribe all go through
 * the PAL.  Behavior preserved verbatim — same retry shape, same
 * exclusion list, same status-notify wait.  Diagnostic shell command
 * only (no production call sites).
 */

#include <nn_osal/osal.h>
#include <node_mgr/provision_manager.h>
#include <node_mgr/network_manager.h>

#include <errno.h>
#include <string.h>

#include <nn_pal/ble.h>

#include <openthread/dataset.h>

NN_OSAL_LOG_MODULE(prov_central);

/* ---------- UUIDs ----------------------------------------------------- */

#define _U(low4) { \
	0xf0,0xa2,0xd5,0xd0,0x26,0x3e,0x32,0x92, \
	0x6b,0x4f,0x3e,0x6b, (low4) & 0xff, ((low4) >> 8) & 0xff, 0xf0, 0xe7 }
static const nn_pal_ble_uuid_t UUID_PROV_SVC     = { .bytes = _U(0x0001) };
static const nn_pal_ble_uuid_t UUID_PROV_DATASET = { .bytes = _U(0x0002) };
static const nn_pal_ble_uuid_t UUID_PROV_STATUS  = { .bytes = _U(0x0003) };
#undef _U

/* ---------- state ------------------------------------------------------ */

#define STATUS_SUCCESS 0x02
#define STATUS_ERROR   0x03

static nn_pal_ble_conn_t g_conn;
static bool              g_conn_failed;

/* Discovered handles for the two chrcs we use. */
static uint16_t g_dataset_handle;
static uint16_t g_status_handle;
static uint16_t g_ccc_handle;

static const uint8_t *g_tlvs;
static uint8_t        g_tlv_len;

static K_SEM_DEFINE(connected_sem,   0, 1);
static K_SEM_DEFINE(discovery_sem,   0, 1);
static K_SEM_DEFINE(write_sem,       0, 1);
static K_SEM_DEFINE(prov_result_sem, 0, 1);
static K_SEM_DEFINE(read_sem,        0, 1);

static int g_prov_result;
static int g_disco_err;
static int g_write_err;

/* ---------- GATT discovery -------------------------------------------- */

static void disco_done(nn_pal_ble_conn_t conn, int err, void *user)
{
	ARG_UNUSED(conn); ARG_UNUSED(user);
	g_disco_err = err;
	k_sem_give(&discovery_sem);
}

static int run_discovery(nn_pal_ble_conn_t conn, bool need_ccc)
{
	nn_pal_ble_remote_chrc_t targets[2] = {
		{ .uuid = UUID_PROV_DATASET },
		{ .uuid = UUID_PROV_STATUS  },
	};

	g_dataset_handle = 0;
	g_status_handle  = 0;
	g_ccc_handle     = 0;
	g_disco_err      = 0;
	k_sem_reset(&discovery_sem);

	int err = nn_pal_ble_gatt_discover(conn, &UUID_PROV_SVC,
					   targets, ARRAY_SIZE(targets),
					   disco_done, NULL);
	if (err) {
		NN_LOG_ERR("nn_pal_ble_gatt_discover: %d", err);
		return err;
	}
	if (k_sem_take(&discovery_sem, K_SECONDS(10)) != 0) {
		NN_LOG_ERR("GATT discovery timed out");
		return -ETIMEDOUT;
	}
	if (g_disco_err) {
		NN_LOG_ERR("GATT discovery failed: %d", g_disco_err);
		return g_disco_err;
	}
	g_dataset_handle = targets[0].value_handle;
	g_status_handle  = targets[1].value_handle;
	g_ccc_handle     = targets[1].ccc_handle;

	if (!g_dataset_handle) {
		NN_LOG_ERR("Dataset handle not found");
		return -ENOENT;
	}
	if (need_ccc && !g_ccc_handle) {
		if (g_status_handle) {
			g_ccc_handle = g_status_handle + 1;
			NN_LOG_INF("CCC not discovered, inferred handle: %u",
				   g_ccc_handle);
		} else {
			NN_LOG_ERR("Missing handles: dataset=%u status=%u",
				   g_dataset_handle, g_status_handle);
			return -ENOENT;
		}
	}
	return 0;
}

/* ---------- status notification --------------------------------------- */

static bool status_notify_cb(nn_pal_ble_conn_t conn,
			      const uint8_t *data, size_t length,
			      void *user)
{
	ARG_UNUSED(conn); ARG_UNUSED(user); ARG_UNUSED(length);
	if (!data) {
		NN_LOG_INF("Status notify: unsubscribed");
		return false;
	}
	uint8_t status = data[0];
	NN_LOG_INF("Status notify: 0x%02x", status);
	if (status == STATUS_SUCCESS) {
		g_prov_result = 0;
		k_sem_give(&prov_result_sem);
	} else if (status == STATUS_ERROR) {
		g_prov_result = -EIO;
		k_sem_give(&prov_result_sem);
	}
	return true;
}

static int subscribe_status(nn_pal_ble_conn_t conn)
{
	int err = nn_pal_ble_gatt_subscribe(conn, g_status_handle, g_ccc_handle,
					    status_notify_cb, NULL);
	if (err && err != -EALREADY) {
		NN_LOG_ERR("nn_pal_ble_gatt_subscribe: %d", err);
		return err;
	}
	NN_LOG_INF("Subscribed to status notifications");
	return 0;
}

/* ---------- write dataset --------------------------------------------- */

static void write_done(nn_pal_ble_conn_t conn, int err, void *user)
{
	ARG_UNUSED(conn); ARG_UNUSED(user);
	if (err) NN_LOG_ERR("Dataset write failed: %d", err);
	else     NN_LOG_INF("Dataset write acknowledged");
	g_write_err = err;
	k_sem_give(&write_sem);
}

static int write_dataset(nn_pal_ble_conn_t conn)
{
	g_write_err = 0;
	k_sem_reset(&write_sem);
	int err = nn_pal_ble_gatt_write(conn, g_dataset_handle,
					g_tlvs, g_tlv_len,
					write_done, NULL);
	if (err) {
		NN_LOG_ERR("nn_pal_ble_gatt_write: %d", err);
		return err;
	}
	if (k_sem_take(&write_sem, K_SECONDS(10)) != 0) {
		NN_LOG_ERR("Dataset write timed out");
		return -ETIMEDOUT;
	}
	return g_write_err;
}

/* ---------- read dataset ---------------------------------------------- */

static uint8_t *g_read_buf;
static uint8_t  g_read_buf_max;
static uint8_t  g_read_len;
static int      g_read_err;

static void read_done(nn_pal_ble_conn_t conn, int err,
		      const uint8_t *data, size_t length, void *user)
{
	ARG_UNUSED(conn); ARG_UNUSED(user);
	if (err) {
		NN_LOG_ERR("Dataset read failed: %d", err);
		g_read_err = err;
		k_sem_give(&read_sem);
		return;
	}
	size_t copy = length;
	if (copy > g_read_buf_max) copy = g_read_buf_max;
	memcpy(g_read_buf, data, copy);
	g_read_len = (uint8_t)copy;
	NN_LOG_INF("Dataset read: %u bytes", g_read_len);
	g_read_err = 0;
	k_sem_give(&read_sem);
}

static int read_dataset(nn_pal_ble_conn_t conn)
{
	g_read_err = 0;
	g_read_len = 0;
	k_sem_reset(&read_sem);
	int err = nn_pal_ble_gatt_read(conn, g_dataset_handle,
				       read_done, NULL);
	if (err) {
		NN_LOG_ERR("nn_pal_ble_gatt_read: %d", err);
		return err;
	}
	if (k_sem_take(&read_sem, K_SECONDS(10)) != 0) {
		NN_LOG_ERR("Dataset read timed out");
		return -ETIMEDOUT;
	}
	if (g_read_err) return g_read_err;
	if (g_read_len == 0) {
		NN_LOG_ERR("Received zero-length dataset");
		return -ENODATA;
	}
	return 0;
}

/* ---------- scan ------------------------------------------------------ */

static bool g_scan_found;
static uint8_t g_peer_addr[6];
static uint8_t g_peer_addr_type;

/* Addresses to skip: joiners with no dataset (during fetch) or the
 * leader (during push). */
struct addr_le { uint8_t a[6]; uint8_t type; };
static struct addr_le g_excluded[4];
static int            g_n_excluded;

static bool is_excluded(const uint8_t *addr, uint8_t type)
{
	for (int i = 0; i < g_n_excluded; i++) {
		if (g_excluded[i].type == type &&
		    memcmp(g_excluded[i].a, addr, 6) == 0) {
			return true;
		}
	}
	return false;
}

static void scan_cb(const nn_pal_ble_scan_result_t *r, void *user)
{
	ARG_UNUSED(user);
	if (g_scan_found) return;
	if (is_excluded(r->addr, r->addr_type)) return;

	NN_LOG_INF("BLE device: %02x:%02x:%02x:%02x:%02x:%02x type=%u rssi=%d uuids=%zu",
		r->addr[5], r->addr[4], r->addr[3],
		r->addr[2], r->addr[1], r->addr[0],
		r->addr_type, r->rssi_dbm, r->adv_uuid_count);

	bool found = false;
	for (size_t i = 0; i < r->adv_uuid_count; i++) {
		if (memcmp(r->adv_uuids[i].bytes, UUID_PROV_SVC.bytes, 16) == 0) {
			found = true;
			break;
		}
	}
	if (!found) return;

	NN_LOG_INF("Found OT-Node peripheral");
	g_scan_found     = true;
	g_peer_addr_type = r->addr_type;
	memcpy(g_peer_addr, r->addr, 6);
	(void)nn_pal_ble_scan_stop();
	k_sem_give(&connected_sem);
}

/* ---------- connection callback --------------------------------------- */

static void central_conn_cb(nn_pal_ble_conn_t conn,
			    nn_pal_ble_conn_event_t ev,
			    const nn_pal_ble_conn_info_t *info, void *user)
{
	ARG_UNUSED(info); ARG_UNUSED(user);
	switch (ev) {
	case NN_PAL_BLE_CONN_EV_CONNECTED:
		NN_LOG_INF("Central: connected to peripheral");
		g_conn = conn;
		g_conn_failed = false;
		k_sem_give(&connected_sem);
		break;
	case NN_PAL_BLE_CONN_EV_DISCONNECTED:
		NN_LOG_INF("Central: disconnected (reason %u)",
			info ? info->disconnect_reason : 0);
		if (g_conn == conn) {
			g_conn = 0;
			g_conn_failed = true;
			/* Wake the connect-wait if we were still waiting. */
			k_sem_give(&connected_sem);
		}
		break;
	default:
		break;
	}
}

static bool g_cb_registered;

static void ensure_cb_registered(void)
{
	if (g_cb_registered) return;
	(void)nn_pal_ble_conn_cb_register(central_conn_cb, NULL);
	g_cb_registered = true;
}

/* ---------- public API ------------------------------------------------ */

int prov_central_fetch(uint8_t *out_tlvs, uint8_t *out_len,
		       uint32_t timeout_ms)
{
	int err;

	if (!out_tlvs || !out_len) {
		return -EINVAL;
	}

	/* Free the shared RF for BLE */
	nm_ble_acquire_rf();
	ensure_cb_registered();

	g_n_excluded = 0;

	/*
	 * Retry loop: if we connect to a joiner (no dataset loaded),
	 * exclude it and try again until we find the leader.
	 */
	for (int attempt = 0; attempt < 4; attempt++) {
		k_sem_reset(&connected_sem);
		k_sem_reset(&discovery_sem);
		k_sem_reset(&read_sem);
		g_scan_found   = false;
		g_read_buf     = out_tlvs;
		g_read_buf_max = OT_OPERATIONAL_DATASET_MAX_LENGTH;
		g_read_len     = 0;

		NN_LOG_INF("Scanning for leader (attempt %d)...", attempt + 1);
		nn_pal_ble_scan_params_t sp = { .passive = true };
		err = nn_pal_ble_scan_start(&sp, scan_cb, NULL);
		if (err) {
			NN_LOG_ERR("Scan start failed: %d", err);
			return err;
		}

		if (k_sem_take(&connected_sem, K_MSEC(timeout_ms)) != 0) {
			(void)nn_pal_ble_scan_stop();
			NN_LOG_ERR("Scan timed out — no leader found");
			return -ETIMEDOUT;
		}

		if (!g_scan_found) {
			return -ENODEV;
		}

		NN_LOG_INF("Connecting to leader candidate...");
		k_sem_reset(&connected_sem);
		g_conn_failed = false;

		err = nn_pal_ble_connect(g_peer_addr, g_peer_addr_type);
		if (err) {
			NN_LOG_ERR("nn_pal_ble_connect: %d", err);
			return err;
		}

		if (k_sem_take(&connected_sem, K_SECONDS(10)) != 0) {
			NN_LOG_ERR("Connection timed out");
			return -ETIMEDOUT;
		}
		if (!g_conn || g_conn_failed) {
			return -ECONNREFUSED;
		}

		(void)nn_pal_ble_gatt_mtu_exchange(g_conn);

		NN_LOG_INF("Discovering dataset service...");
		err = run_discovery(g_conn, false);
		if (err) {
			(void)nn_pal_ble_conn_disconnect(g_conn);
			return err;
		}

		NN_LOG_INF("Reading dataset...");
		err = read_dataset(g_conn);

		if (err == -ENODATA &&
		    g_n_excluded < (int)ARRAY_SIZE(g_excluded)) {
			NN_LOG_WRN("Device has no dataset — excluding and retrying");
			memcpy(g_excluded[g_n_excluded].a, g_peer_addr, 6);
			g_excluded[g_n_excluded].type = g_peer_addr_type;
			g_n_excluded++;
			(void)nn_pal_ble_conn_disconnect(g_conn);
			nn_osal_sleep_ms(500);
			continue;
		}

		(void)nn_pal_ble_conn_disconnect(g_conn);
		break;
	}

	if (err == 0) {
		*out_len = g_read_len;
		NN_LOG_INF("Fetched dataset: %u bytes from leader", g_read_len);
		/* Carry the leader's address as the exclusion for the upcoming
		 * prov_central_push() scan so it skips the leader and finds
		 * the joiner. */
		g_n_excluded = 1;
		memcpy(g_excluded[0].a, g_peer_addr, 6);
		g_excluded[0].type = g_peer_addr_type;
	} else {
		NN_LOG_ERR("Dataset fetch failed: %d", err);
	}
	return err;
}

int prov_central_push(const uint8_t *tlvs, uint8_t tlv_len,
		      uint32_t timeout_ms)
{
	int err;

	g_tlvs    = tlvs;
	g_tlv_len = tlv_len;

	k_sem_reset(&connected_sem);
	k_sem_reset(&discovery_sem);
	k_sem_reset(&write_sem);
	k_sem_reset(&prov_result_sem);
	g_scan_found  = false;
	g_prov_result = -EIO;
	/* g_n_excluded carries the leader's address from fetch — do not reset */

	nm_ble_acquire_rf();
	ensure_cb_registered();

	NN_LOG_INF("Scanning for joiner peripheral...");
	nn_pal_ble_scan_params_t sp = { .passive = true };
	err = nn_pal_ble_scan_start(&sp, scan_cb, NULL);
	if (err) {
		NN_LOG_ERR("Scan start failed: %d", err);
		return err;
	}

	if (k_sem_take(&connected_sem, K_MSEC(timeout_ms)) != 0) {
		(void)nn_pal_ble_scan_stop();
		NN_LOG_ERR("Scan timed out — no joiner found");
		return -ETIMEDOUT;
	}

	if (!g_scan_found) {
		return -ENODEV;
	}

	NN_LOG_INF("Connecting to joiner...");
	k_sem_reset(&connected_sem);
	g_conn_failed = false;

	err = nn_pal_ble_connect(g_peer_addr, g_peer_addr_type);
	if (err) {
		NN_LOG_ERR("nn_pal_ble_connect: %d", err);
		return err;
	}

	if (k_sem_take(&connected_sem, K_SECONDS(10)) != 0) {
		NN_LOG_ERR("Connection timed out");
		return -ETIMEDOUT;
	}
	if (!g_conn || g_conn_failed) {
		return -ECONNREFUSED;
	}

	(void)nn_pal_ble_gatt_mtu_exchange(g_conn);

	NN_LOG_INF("Discovering OT Provisioning Service...");
	err = run_discovery(g_conn, true);
	if (err) {
		goto disconnect;
	}

	err = subscribe_status(g_conn);
	if (err) {
		goto disconnect;
	}

	NN_LOG_INF("Writing dataset (%u bytes)...", tlv_len);
	err = write_dataset(g_conn);
	if (err) {
		goto disconnect;
	}

	if (k_sem_take(&prov_result_sem, K_SECONDS(30)) != 0) {
		NN_LOG_ERR("Timed out waiting for provisioning result");
		err = -ETIMEDOUT;
		goto disconnect;
	}
	err = g_prov_result;

disconnect:
	if (g_conn) {
		(void)nn_pal_ble_conn_disconnect(g_conn);
	}

	if (err == 0) {
		NN_LOG_INF("Joiner provisioned successfully");
	} else {
		NN_LOG_ERR("Provisioning failed: %d", err);
	}
	return err;
}
