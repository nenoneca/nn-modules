/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * ota_client — device-initiated firmware OTA over CoAP (plain, no ECIES).
 *
 * Flow:
 *   1. ota_client_check()    POST /ota/check → hub responds with update metadata
 *   2. ota_client_download() GET  /ota/image  → Block2 stream → flash slot1
 *   3. ota_client_apply()    boot_request_upgrade() + sys_reboot()
 *
 * The hub CoAP address is set via ota_client_set_hub_addr() (shell / NVS).
 * Auto-apply policy is persisted in NVS at "ota/auto_apply".
 */

/** Initialise OTA client subsystem. Call after settings_load(). */
int ota_client_init(void);

/** Diagnostic: emit LOG_INF/WRN/ERR from this TU. */
void ota_client_logtest(const char *which);

/** Set hub CoAP IPv6 address. Persisted to NVS. */
int ota_client_set_hub_addr(const char *addr);

/** Get current hub CoAP address (NUL-terminated, or "" if unset). */
const char *ota_client_get_hub_addr(void);

/**
 * Check hub for available firmware update.
 *
 * @return  1 if update available (metadata stored internally)
 *          0 if up-to-date
 *         <0 on error (network, parse, etc.)
 */
int ota_client_check(void);

/**
 * Download firmware from hub via CoAP Block2 and write to flash slot1.
 * Must call ota_client_check() first (needs metadata).
 *
 * @return  0 on success (image in slot1, SHA256 verified)
 *         <0 on error
 */
int ota_client_download(void);

/**
 * Apply downloaded firmware: boot_request_upgrade(TEST) + reboot.
 * The new image boots in test mode — call ota_client_confirm() to
 * make it permanent. Does not return on success.
 *
 * @return <0 on error (no image in slot1, etc.)
 */
int ota_client_apply(void);

/**
 * Confirm the running image as permanent.
 * Call after a successful boot to prevent MCUboot from reverting.
 * Safe to call on already-confirmed images (no-op).
 *
 * @return 0 on success, <0 on error
 */
int ota_client_confirm(void);

/** Set auto-apply policy. Persisted to NVS. */
void ota_client_set_auto_apply(bool enable);

/** Get auto-apply policy. */
bool ota_client_get_auto_apply(void);

/** Get pending update version string (valid after successful check). */
const char *ota_client_pending_version(void);

/** Get pending update size (valid after successful check). */
uint32_t ota_client_pending_size(void);


/** Arm the downloaded image: persist the pending version to KV and
 *  report OTA_READY to the hub.  The image is applied later, on an
 *  operator-confirmed OTA_APPLY (fleet-coordinated reboot) — see
 *  ota_client_apply().  Requires a successful download first. */
int ota_client_arm(void);

/** Armed target version ("" when not armed).  Surfaced in INFO_REPLY
 *  so the hub can reliably learn the armed state even if the
 *  fire-and-forget OTA_READY frame is lost. */
const char *ota_client_armed_version(void);
