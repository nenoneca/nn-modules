/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

/*
 * init_manager — startup policy for BLE+Thread nodes.
 *
 * Determines at boot whether the device has a persisted Thread dataset
 * (and can rejoin directly via nm_thread_start()) or needs to go through
 * the BLE provisioning flow first.
 *
 * Typical joiner startup:
 *
 *   if (im_needs_provisioning()) {
 *       nm_ble_acquire_rf();
 *       prov_peripheral_start();
 *       prov_peripheral_wait(5 * 60 * 1000);
 *       prov_peripheral_stop();
 *       // Thread started inside the GATT write handler
 *   } else {
 *       nm_thread_start();
 *       nm_thread_wait_for_role(OT_DEVICE_ROLE_CHILD, 60000);
 *   }
 */

/**
 * Returns true if the device has NOT been provisioned and must go through
 * the BLE provisioning flow before starting Thread.
 *
 * Returns false if a Thread dataset was previously received and persisted —
 * the device can rejoin directly with nm_thread_start().
 */
bool im_needs_provisioning(void);

/**
 * Clear the persisted provisioning flag and stop Thread.
 * After this call, im_needs_provisioning() returns true and the next
 * start_joiner invocation will go through BLE provisioning again.
 */
void im_clear_provisioning(void);

/**
 * Boot-time decision: if the device is fully provisioned (persistent
 * state machine in PROV_STATE_PROVISIONED AND OT confirms the active
 * dataset), auto-start Thread.  Otherwise return without starting
 * Thread and let the caller fall back to BLE provisioning.
 *
 * Returns 0 if Thread was started, -ENOENT if the device needs BLE
 * provisioning, or another negative errno on Thread-start failure.
 */
int im_boot_auto_attach(void);
