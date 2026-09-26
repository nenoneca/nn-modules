/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * provision_manager — BLE GATT peripheral (dataset receiver/provider) and
 * BLE GATT central (dataset fetcher/pusher) for Thread provisioning.
 *
 * Peripheral API (Nodes 1 and 2):
 *   Node 1 (leader):  prov_set_leader_dataset() then prov_peripheral_start()
 *   Node 2 (joiner):  prov_peripheral_start(), prov_peripheral_wait()
 *
 * Central API (Node 3 broker):
 *   prov_central_fetch() — scan + connect + READ dataset from leader
 *   prov_central_push()  — scan + connect + WRITE dataset to joiner
 */

/* ---------- Peripheral -------------------------------------------------- */

/**
 * Set the dataset TLVs to expose via the readable GATT dataset characteristic.
 * Call this before prov_peripheral_start() on the leader node (Node 1).
 */
void prov_set_leader_dataset(const uint8_t *tlvs, uint8_t len);

/** Start advertising the OT Provisioning Service. */
int prov_peripheral_start(void);

/** Stop advertising (call after provisioning is complete). */
void prov_peripheral_stop(void);

/**
 * Block until a dataset has been received and applied, or until
 * @p timeout_ms elapses.
 * Returns 0 on success, -ETIMEDOUT if no provisioning occurred.
 */
int prov_peripheral_wait(uint32_t timeout_ms);

/** True if a dataset has been received and applied (persisted in flash). */
bool prov_is_provisioned(void);

/** Clear the persisted provisioning flag from flash. */
void prov_clear_flag(void);

/* ---------- Central ----------------------------------------------------- */

/**
 * Scan for the leader peripheral, connect, and READ the dataset.
 * Automatically excludes joiners that return READ_NOT_PERMITTED (no dataset).
 * On success, the leader's address is retained to prevent the upcoming
 * prov_central_push() scan from connecting to the leader again.
 *
 * @param out_tlvs   buffer to receive TLVs (≥ OT_OPERATIONAL_DATASET_MAX_LENGTH)
 * @param out_len    receives the number of bytes written
 * @param timeout_ms scan + connect timeout
 *
 * Returns 0 on success, negative errno on error/timeout.
 */
int prov_central_fetch(uint8_t *out_tlvs, uint8_t *out_len, uint32_t timeout_ms);

/**
 * Scan for a joiner peripheral, connect, WRITE the dataset, and wait for
 * the STATUS_SUCCESS notification confirming the joiner applied it.
 *
 * @param tlvs       dataset TLV bytes (from prov_central_fetch)
 * @param len        number of bytes
 * @param timeout_ms scan + connect + provision timeout
 *
 * Returns 0 on success, negative errno on error/timeout.
 */
int prov_central_push(const uint8_t *tlvs, uint8_t len, uint32_t timeout_ms);
