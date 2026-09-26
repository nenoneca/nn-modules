/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * time_sync — Hub-synchronized wall-clock time.
 *
 * Periodically pulls epoch from hub via CoAP GET /time.
 * Stores offset: hub_epoch_ms - local_uptime_ms.
 * Provides time_sync_now_ms() that returns current wall-clock ms.
 *
 * Uses the hub address from ota_client (NVS "ota/hub_addr").
 */

/** Initialise time sync (no-op until first sync). */
void time_sync_init(void);

/** Trigger a single sync request to the hub. */
int time_sync_once(void);

/**
 * Get current wall-clock time in milliseconds since Unix epoch.
 * Returns 0 if never synced.
 */
int64_t time_sync_now_ms(void);

/** Get milliseconds since last successful sync. */
int64_t time_sync_age_ms(void);

/** True if at least one sync has succeeded. */
bool time_sync_is_valid(void);
