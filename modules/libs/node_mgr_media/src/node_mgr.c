/* SPDX-License-Identifier: Apache-2.0 */
#include "node_mgr_media/node_mgr_media.h"
#include "nn_registry/nn_features.h"
#include "nn_link/nn_link.h"
#if NN_HAS_P4CTL
#  include "nn_p4ctl/nn_p4ctl.h"
#endif
#if NN_HAS_BLE && NN_HAS_WIFI
#  include "nn_prov/nn_prov.h"
#  include "nn_netstream/nn_netstream.h"
#  include "nn_ctrl/nn_ctrl.h"
#endif
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(node_mgr_media);


esp_err_t node_mgr_init(void)
{
    NN_LOG_INF("node_mgr (esp-idf) starting: %s", nn_registry_summary());

    esp_err_t ret = nn_link_init();
    if (ret != ESP_OK) return ret;

#if NN_HAS_P4CTL
    ret = nn_p4ctl_init();
    if (ret != ESP_OK) return ret;
#endif

    nn_link_cli_register();
#if NN_HAS_P4CTL
    nn_p4ctl_cli_register();
#endif

#if NN_HAS_BLE && NN_HAS_WIFI
    /* Wi-Fi/TCP video uplink + BLE provisioning.  Init order matters:
     * nn_netstream must be ready before nn_prov can apply a restored config. */
    ret = nn_netstream_init();
    if (ret != ESP_OK) return ret;
    ret = nn_prov_init();             /* loads/generates keys, restores NVS config */
    if (ret != ESP_OK) return ret;
    nn_netstream_cli_register();      /* `net` */
    nn_prov_cli_register();           /* `prov` */
    nn_ctrl_init();                   /* `ctrl` */
#endif
    return ESP_OK;
}

esp_err_t node_mgr_start(void)
{
    /* Order matters on the C6: the SDIO slave must be LISTENING before the P4
     * is released from reset, otherwise the P4 starts probing an absent slave.
     * So: start the slave first, THEN power-cycle the P4 into a ready bus. */
    esp_err_t ret = nn_link_start();
    if (ret != ESP_OK) {
        NN_LOG_ERR("nn_link_start: %s", esp_err_to_name(ret));
        return ret;
    }

#if NN_HAS_P4CTL
    NN_LOG_INF("slave listening; power-cycling P4 so it initiates the link");
    nn_p4ctl_power_cycle();
    NN_LOG_INF("waiting for P4 master to connect...");
#endif

#if NN_HAS_BLE && NN_HAS_WIFI
    /* BLE is provisioning-only.  Mirror the sensor/gateway strategy: enable the
     * BT radio *only* when we actually need to provision.  Once provisioned, the
     * NimBLE host (and therefore the BT controller) is never started, so there is
     * no Wi-Fi/BLE software coexistence stealing radio airtime — the encrypted
     * video uplink gets the full radio.  (Coex was dropping uplink TCP segments,
     * even SYN-ACKs, forcing the stream to reconnect every ~20 s.)
     *
     * To re-provision, clear the stored config (`prov reset`) and reboot: the
     * device then boots unprovisioned and brings BLE back up to advertise. */
    if (nn_prov_is_provisioned()) {
        NN_LOG_INF("already provisioned — Wi-Fi uplink only, BLE stays off");
        nn_netstream_start();

        ret = nn_ctrl_start();        /* encrypted hub control channel (Wi-Fi) */
        if (ret != ESP_OK) NN_LOG_WRN("nn_ctrl_start: %s", esp_err_to_name(ret));
    } else {
        NN_LOG_INF("unprovisioned — enabling BLE to advertise for provisioning");
        ret = nn_prov_start();        /* NimBLE advertise (enables BT controller) */
        if (ret != ESP_OK) NN_LOG_WRN("nn_prov_start: %s", esp_err_to_name(ret));
    }
#endif
    return ESP_OK;
}
