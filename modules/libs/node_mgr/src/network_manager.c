/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/osal.h>
#include <node_mgr/network_manager.h>

#include <errno.h>
#include <string.h>
#include <nn_pal/openthread.h>
#include <nn_pal/ble.h>

#include <openthread/dataset.h>
#include <openthread/dataset_ftd.h>
#include <openthread/ip6.h>
#include <openthread/thread.h>

#if defined(CONFIG_ESP32_SW_COEXIST_ENABLE)
#include "esp_coex_i154.h"
#endif
#include "esp_ieee802154.h"
#include "esp_phy_init.h"

NN_OSAL_LOG_MODULE(network_mgr);

/*
 * ESP32-C6 BLE + 802.15.4 coexistence — PHY pre-initialisation
 * =============================================================
 *
 * The ESP32-C6 shares one RF front-end between BLE and 802.15.4.
 * Zephyr's net_if_post_init() puts the 802.15.4 MAC into continuous RX at
 * boot, blocking BLE scans entirely.  Fix:
 *
 *   1. SYS_INIT at POST_KERNEL priority 75 calls esp_phy_enable(PHY_MODEM_BT)
 *      before the 802.15.4 driver (priority 80), giving BLE the full
 *      calibration path.
 *
 *   2. nm_ble_acquire_rf() calls esp_ieee802154_disable() before any BLE
 *      scan or advertising.  This fully releases the PHY.
 *
 *   3. maybe_wake_radio() calls esp_ieee802154_enable() before Thread starts.
 */
static int phy_ble_first_caller_init(void)
{
	esp_phy_enable(PHY_MODEM_BT);
	return 0;
}
/* Priority 75 < IEEE802154_ESP32_INIT_PRIO (80) */
NN_OSAL_INIT_EARLY(phy_ble_first_caller_init, 75);

/* ---------- state ------------------------------------------------------ */

static bool g_radio_disabled;
static bool g_nm_ready;

/* ---------- internal helpers ------------------------------------------ */

static void maybe_wake_radio(void)
{
	if (!g_radio_disabled) {
		return;
	}
	NN_LOG_INF("Re-enabling 802.15.4 before Thread start...");
	esp_ieee802154_enable();
	g_radio_disabled = false;
	NN_LOG_INF("802.15.4 re-enabled");
}

static otInstance *get_inst(void)
{
	return (otInstance *)nn_pal_ot_instance();
}

static int ot_start_iface(void)
{
	return nn_pal_ot_iface_start();
}

static void coex_set_idle(void)
{
#if defined(CONFIG_ESP32_SW_COEXIST_ENABLE)
	esp_coex_ieee802154_txrx_pti_set(IEEE802154_IDLE);
	esp_coex_ieee802154_ack_pti_set(IEEE802154_MIDDLE);
	NN_LOG_INF("802.15.4 coex PTI set to IDLE");
#endif
}

/* ---------- public API ------------------------------------------------- */

int nm_init(void)
{
	/* nn_pal_ble_init blocks until the BT host's bt_ready callback
	 * fires, so by the time it returns we can immediately tune coex
	 * and report ready. */
	/* Bluetooth is NOT started here any more: a registered sensor never
	 * needs it (it is only the provisioning channel), and it shares the
	 * one RF front-end with 802.15.4.  It is started on demand by
	 * nm_ble_acquire_rf(), which every BLE user (setup-mode advertising,
	 * provisioning central) already calls first.  Tearing it down after
	 * boot instead (bt_disable) panicked in the ESP32-C6 controller deinit
	 * (modem_clock_bt_wifipwr_clk_workaround), seen on c6-s1 2026-09-24. */
	g_nm_ready = true;
	NN_LOG_INF("Bluetooth ready");
	return 0;
}

bool nm_is_ready(void)
{
	return g_nm_ready;
}

void nm_ble_acquire_rf(void)
{
	int berr = nn_pal_ble_init();       /* idempotent; blocks until ready */
	if (berr) {
		NN_LOG_ERR("nn_pal_ble_init failed: %d", berr);
	} else {
		coex_set_idle();
	}
	if (g_radio_disabled) {
		NN_LOG_INF("802.15.4 already disabled");
		return;
	}
	NN_LOG_INF("Disabling 802.15.4 to free PHY for BLE...");
	esp_ieee802154_disable();
	g_radio_disabled = true;
	NN_LOG_INF("802.15.4 PHY disabled — BLE ready");
}

int nm_thread_create_network(void)
{
	nn_pal_ot_mutex_lock();

	otInstance *inst = get_inst();
	otOperationalDataset dataset = {0};

	otError err = otDatasetCreateNewNetwork(inst, &dataset);
	if (err != OT_ERROR_NONE) {
		NN_LOG_ERR("otDatasetCreateNewNetwork failed: %d", err);
		nn_pal_ot_mutex_unlock();
		return -EIO;
	}

	err = otDatasetSetActive(inst, &dataset);
	if (err != OT_ERROR_NONE) {
		NN_LOG_ERR("otDatasetSetActive failed: %d", err);
		nn_pal_ot_mutex_unlock();
		return -EIO;
	}

	nn_pal_ot_mutex_unlock();

	maybe_wake_radio();

	nn_pal_ot_mutex_lock();
	int rc = ot_start_iface();
	nn_pal_ot_mutex_unlock();

	if (rc == 0) {
		NN_LOG_INF("Thread network created — waiting for Leader role");
	}
	return rc;
}

int nm_thread_start(void)
{
	maybe_wake_radio();
	nn_pal_ot_mutex_lock();
	int rc = ot_start_iface();
	nn_pal_ot_mutex_unlock();
	if (rc == 0) {
		NN_LOG_INF("Thread started from stored dataset");
	}
	return rc;
}

int nm_thread_apply_dataset(const uint8_t *tlvs, uint8_t len)
{
	if (!tlvs || len == 0 || len > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
		return -EINVAL;
	}

	nn_pal_ot_mutex_lock();

	otInstance *inst = get_inst();
	otOperationalDatasetTlvs ds_tlvs;

	memcpy(ds_tlvs.mTlvs, tlvs, len);
	ds_tlvs.mLength = len;

	otError err = otDatasetSetActiveTlvs(inst, &ds_tlvs);
	if (err != OT_ERROR_NONE) {
		NN_LOG_ERR("otDatasetSetActiveTlvs failed: %d", err);
		nn_pal_ot_mutex_unlock();
		return -EIO;
	}

	nn_pal_ot_mutex_unlock();

	maybe_wake_radio();

	nn_pal_ot_mutex_lock();
	int rc = ot_start_iface();
	nn_pal_ot_mutex_unlock();

	if (rc == 0) {
		NN_LOG_INF("Dataset applied — Thread started");
	}
	return rc;
}

int nm_thread_get_dataset(uint8_t *out_tlvs, uint8_t *out_len)
{
	if (!out_tlvs || !out_len) {
		return -EINVAL;
	}

	nn_pal_ot_mutex_lock();

	otOperationalDatasetTlvs ds_tlvs;
	otError err = otDatasetGetActiveTlvs(get_inst(), &ds_tlvs);

	nn_pal_ot_mutex_unlock();

	if (err != OT_ERROR_NONE) {
		NN_LOG_ERR("otDatasetGetActiveTlvs failed: %d", err);
		return -EIO;
	}

	memcpy(out_tlvs, ds_tlvs.mTlvs, ds_tlvs.mLength);
	*out_len = (uint8_t)ds_tlvs.mLength;
	return 0;
}

otDeviceRole nm_thread_get_role(void)
{
	nn_pal_ot_mutex_lock();
	otDeviceRole role = otThreadGetDeviceRole(get_inst());
	nn_pal_ot_mutex_unlock();
	return role;
}

int nm_thread_wait_for_role(otDeviceRole target, uint32_t timeout_ms)
{
	uint32_t elapsed = 0;
	const uint32_t step = 500;

	while (elapsed < timeout_ms) {
		if (nm_thread_get_role() == target) {
			return 0;
		}
		nn_osal_sleep_ms(step);
		elapsed += step;
	}
	return -ETIMEDOUT;
}

void nm_thread_stop(void)
{
	(void)nn_pal_ot_iface_stop();
	NN_LOG_INF("Thread stopped");
}
