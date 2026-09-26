/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "nn_prov/nn_prov.h"

/* Internal contract between the nn_prov core (nn_prov.c) and the BLE GATT
 * layer (nn_prov_ble.c). */

/* Core handlers, invoked by the GATT write callbacks. */
esp_err_t nn_prov_handle_config(const uint8_t *data, size_t len);  /* plaintext CONFIG */
esp_err_t nn_prov_handle_wifi(const uint8_t *data, size_t len);    /* ECIES WIFI envelope */

/* Current status (for the STATUS read handler). */
nn_prov_status_t nn_prov_current_status(void);

/* Implemented by the BLE layer; called by the core to push a STATUS notify. */
void nn_prov_ble_notify_status(nn_prov_status_t st);

/* Implemented by the BLE layer; bring up NimBLE + the GATT service. */
esp_err_t nn_prov_ble_start(void);
