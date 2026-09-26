/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fw_common Zephyr GATT backend for gw_ble_prov.  Apps include this and
 * call gw_ble_prov_zephyr_start() / _stop() — same shape as the legacy
 * gw_ble_provision_{start,stop}() public API.
 */

#ifndef FW_COMMON_GW_BLE_PROV_ZEPHYR_H_
#define FW_COMMON_GW_BLE_PROV_ZEPHYR_H_

#ifdef __cplusplus
extern "C" {
#endif

int gw_ble_prov_zephyr_start(void);
int gw_ble_prov_zephyr_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* FW_COMMON_GW_BLE_PROV_ZEPHYR_H_ */
