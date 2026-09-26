/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

/*
 * provision_state — persistent state machine that decides at every
 * boot whether the sensor should auto-attach to the Thread mesh or
 * fall back to BLE provisioning.
 *
 * The historical "ble_prov/done" flag (provision_peripheral.c) doesn't
 * always survive reboots — see feedback_mesh_attach_after_reboot.md.
 * This module replaces that single bit with an explicit enum and adds
 * a defense-in-depth check against otDatasetIsCommissioned() so even a
 * corrupted flag won't cause us to BLE-advertise when we already have
 * a working dataset.
 *
 * State machine:
 *
 *   factory boot ─► SETUP ──── BLE pairing commits dataset+hub key ────► PROVISIONED
 *                     │                                                       │
 *                     │ <─── explicit unprovision (im_clear_provisioning) ────┘
 *                     │
 *                     └─► sit advertising the provisioning service
 *
 *   subsequent boot in PROVISIONED ─► otDatasetIsCommissioned()?
 *                                         yes ──► nm_thread_start()
 *                                         no  ──► transition back to SETUP
 *                                                 + BLE advertise
 */

typedef enum {
    PROV_STATE_SETUP       = 0,  /* factory fresh — needs BLE pairing */
    PROV_STATE_PROVISIONED = 1,  /* has Thread dataset + hub key */
} prov_state_t;

/**
 * Load the persisted state from NVS.  Idempotent — safe to call
 * multiple times.  Must be called after settings_load() has run so the
 * NVS-backed handler has populated the cache.
 */
int  prov_state_init(void);

/** Read current state (in-memory mirror of the NVS value). */
prov_state_t prov_state_get(void);

/**
 * Set + persist the state.  The BLE-provisioning success path calls
 * this with PROVISIONED; an explicit factory-reset path (or
 * im_clear_provisioning) calls it with SETUP.
 */
int  prov_state_set(prov_state_t s);

/**
 * Composite boot-time decision: returns true iff we should attempt to
 * auto-attach Thread without going through BLE provisioning first.
 *
 * Requires BOTH:
 *   - persisted state == PROVISIONED
 *   - otDatasetIsCommissioned() (i.e. NVS holds an Active Dataset that
 *     OT actually parsed and considers valid)
 *
 * If either is missing the function returns false and the caller
 * should fall back to BLE provisioning.  Also self-corrects the
 * state: when the flag says PROVISIONED but OT disagrees, the state
 * is forced back to SETUP so the next boot doesn't keep checking.
 */
bool prov_state_should_auto_attach(void);
