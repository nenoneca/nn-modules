/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <openthread/thread.h>

/*
 * network_manager — BLE stack init, RF coexistence, and OpenThread operations.
 *
 * Wraps esp_ieee802154 RF management and the OpenThread Zephyr API so that
 * the application layer never touches radio or OT internals directly.
 */

/**
 * Initialize the BLE stack (bt_enable + coex PTI setup).
 * Blocks until Bluetooth is ready or fails.
 * Returns 0 on success, negative errno on error.
 */
int nm_init(void);

/** True if nm_init() completed successfully. */
bool nm_is_ready(void);

/**
 * Fully disable 802.15.4 to release the shared ESP32-C6 RF front-end for
 * exclusive BLE use.  Must be called before any BLE scan or advertising.
 * Thread start will automatically re-enable the radio.
 */
void nm_ble_acquire_rf(void);

/**
 * Create a brand-new Thread network with a random dataset and start the
 * Thread stack.  The device will become Leader after a short delay.
 */
int nm_thread_create_network(void);

/**
 * Start Thread using whatever dataset the OT settings back-end has stored.
 * Use this after a reboot when the device has already been provisioned.
 */
int nm_thread_start(void);

/**
 * Apply an active-dataset TLV blob received over BLE, persist it via the
 * OT settings back-end, and start the Thread stack.
 * Called from the GATT write k_work handler in provision_peripheral.
 *
 * @param tlvs  raw TLV bytes
 * @param len   number of bytes (≤ OT_OPERATIONAL_DATASET_MAX_LENGTH)
 */
int nm_thread_apply_dataset(const uint8_t *tlvs, uint8_t len);

/**
 * Copy the current active dataset as a TLV blob.
 * Returns 0 on success, negative errno on error.
 */
int nm_thread_get_dataset(uint8_t *out_tlvs, uint8_t *out_len);

/** Return the current Thread device role (OT_DEVICE_ROLE_*). */
otDeviceRole nm_thread_get_role(void);

/**
 * Block until the device reaches @p target role or @p timeout_ms elapses.
 * Returns 0 when the role is reached, -ETIMEDOUT otherwise.
 */
int nm_thread_wait_for_role(otDeviceRole target, uint32_t timeout_ms);

/** Stop Thread and bring the IPv6 interface down. */
void nm_thread_stop(void);
