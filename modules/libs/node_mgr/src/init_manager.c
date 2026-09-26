/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <nn_osal/osal.h>

#include <node_mgr/info_handler.h>
#include <node_mgr/init_manager.h>
#include <node_mgr/network_manager.h>
#include <node_mgr/provision_manager.h>
#include <node_mgr/provision_state.h>
#include <node_mgr/clear_user_data_handler.h>

NN_OSAL_LOG_MODULE(init_manager);

/* Push our config to the hub (firmware version, uptime, capabilities)
 * after Thread auto-attach + gateway HELLO learning.  The first attempt
 * fires at IM_POST_ATTACH_INFO_SYNC_MS post-attach.  On failure (most
 * commonly because the gateway address isn't learned yet — that takes
 * a HELLO mcast cycle, ~5 s, after Thread reaches child role) we
 * reschedule with backoff up to IM_INFO_SYNC_MAX_ATTEMPTS.
 *
 * See feedback_hub_ota_state_running_version_stale: this is what
 * makes the hub's OTA state-machine flip 'applying' → 'done' after a
 * reboot. */
#define IM_POST_ATTACH_INFO_SYNC_MS  10000
#define IM_INFO_SYNC_BACKOFF_MS      10000
#define IM_INFO_SYNC_MAX_ATTEMPTS    6

static nn_osal_work_delayable_t im_info_sync_dw;
static bool im_info_sync_inited;
static uint8_t im_info_sync_attempts;

static void im_post_attach_info_sync_work(nn_osal_work_delayable_t *dw)
{
	ARG_UNUSED(dw);
	im_info_sync_attempts++;
	int rv = info_handler_send_unsolicited();
	if (rv == 0) {
		NN_LOG_INF("post-attach INFO_REPLY sent (attempt %u)",
			   im_info_sync_attempts);
		return;
	}
	if (im_info_sync_attempts >= IM_INFO_SYNC_MAX_ATTEMPTS) {
		NN_LOG_WRN("post-attach INFO_REPLY giving up after %u attempts (last rv=%d)",
			   im_info_sync_attempts, rv);
		return;
	}
	NN_LOG_INF("post-attach INFO_REPLY attempt %u rv=%d, retry in %u ms",
		   im_info_sync_attempts, rv, IM_INFO_SYNC_BACKOFF_MS);
	(void)nn_osal_work_schedule(&im_info_sync_dw, IM_INFO_SYNC_BACKOFF_MS);
}

bool im_needs_provisioning(void)
{
	/* Legacy callers (cmd_start_joiner shell command) still use the
	 * single-bit BLE-prov flag.  Treat it as authoritative for the
	 * "is BLE provisioning needed" question, but the boot-time auto-
	 * attach path (im_boot_auto_attach) uses the richer prov_state
	 * machine which also validates the OT dataset. */
	return !prov_is_provisioned();
}

void im_clear_provisioning(void)
{
	nm_thread_stop();
	prov_clear_flag();
	(void)prov_state_set(PROV_STATE_SETUP);
}

int im_boot_auto_attach(void)
{
	(void)prov_state_init();

	/* Unregister step 2 (after the hub's REBOOT): erase user data and
	 * fall through to setup mode.  Must run BEFORE auto-attach — the
	 * erase needs Thread not-started, and a cleared device must not
	 * rejoin the mesh it was just removed from. */
	if (clear_user_data_boot_check()) {
		NN_LOG_INF("boot: user data cleared — entering setup mode");
		return -ENOENT;
	}

	if (!prov_state_should_auto_attach()) {
		NN_LOG_INF("boot: not auto-attaching (BLE provisioning needed)");
		return -ENOENT;
	}

	int rc = nm_thread_start();
	if (rc) {
		NN_LOG_ERR("boot: nm_thread_start failed: %d", rc);
		return rc;
	}
	NN_LOG_INF("boot: auto-attached Thread (provisioned)");

	/* Push the hub our config (firmware version, capabilities, ...)
	 * once the gateway is reachable.  See info_handler_send_unsolicited. */
	if (!im_info_sync_inited) {
		nn_osal_work_delayable_init(&im_info_sync_dw,
					    im_post_attach_info_sync_work);
		im_info_sync_inited = true;
	}
	im_info_sync_attempts = 0;
	(void)nn_osal_work_schedule(&im_info_sync_dw,
				    IM_POST_ATTACH_INFO_SYNC_MS);
	return 0;
}
