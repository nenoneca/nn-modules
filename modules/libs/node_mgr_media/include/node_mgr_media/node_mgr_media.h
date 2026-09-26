/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_err.h"

/*
 * node_mgr (ESP-IDF port) — Milestone 1 slice.
 *
 * This is the ESP-IDF incarnation of the Zephyr node_mgr library.  For M1 it
 * only wires together the pieces the camera-system bring-up needs:
 *   - the SDIO link (nn_link), role chosen by the registry
 *   - P4 boot/reset control (nn_p4ctl), on the C6 only
 *   - the console commands for both
 *
 * The crypto / nn_proto / provisioning / OTA surfaces of the Zephyr node_mgr
 * are intentionally NOT ported yet; they grow here in later milestones, gated
 * by the same nn_registry feature flags (wifi/ble).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise subsystems for this node's role/features and register all
 * console commands.  Does not bring the SDIO link up. */
esp_err_t node_mgr_init(void);

/* Bring the node to its operational state:
 *   C6 (slave + p4ctl): power-cycle the P4, then start the SDIO slave and
 *                       wait for the P4 master to connect.
 *   P4 (master):        initiate the SDIO link to the C6 slave. */
esp_err_t node_mgr_start(void);

#ifdef __cplusplus
}
#endif
