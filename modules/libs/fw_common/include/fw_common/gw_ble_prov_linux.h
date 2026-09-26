/* SPDX-License-Identifier: Apache-2.0 */
/*
 * BlueZ-based gateway BLE provisioning backend for Linux (RPi 4B).
 *
 * Drives a small sd-bus mainloop that:
 *   - exports our GATT application (service + 8 characteristics)
 *     under /com/nn/gw_prov/
 *   - registers it with bluetoothd via
 *     org.bluez.GattManager1.RegisterApplication
 *   - registers an LE advertisement (service UUID + LocalName
 *     "nn-gw-XXXXXX") via
 *     org.bluez.LEAdvertisingManager1.RegisterAdvertisement
 *
 * Call gw_ble_prov_linux_start() once during daemon startup, then
 * gw_ble_prov_linux_run() to pump the bus event loop until SIGINT/SIGTERM.
 */

#ifndef FW_COMMON_GW_BLE_PROV_LINUX_H_
#define FW_COMMON_GW_BLE_PROV_LINUX_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Start advertising + register GATT app.  `adapter` is the BlueZ adapter
 * D-Bus path, e.g. "/org/bluez/hci0".  Pass NULL for the default
 * (hci0). */
int gw_ble_prov_linux_start(const char *adapter);

/* Run the sd-bus mainloop until gw_ble_prov_linux_stop() is called or
 * a SIGINT/SIGTERM is delivered.  Returns 0 on clean exit. */
int gw_ble_prov_linux_run(void);

/* Tear down advertising + GATT registration.  Safe to call from a
 * signal handler. */
void gw_ble_prov_linux_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* FW_COMMON_GW_BLE_PROV_LINUX_H_ */
