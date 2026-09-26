/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/**
 * Force the chip into ROM download mode on next reset.
 *
 * Sets the ESP32-C6 LP_AON SYS_CFG.FORCE_DOWNLOAD_BOOT bit (RTC retention,
 * preserved across software reset) and issues `sys_reboot(SYS_REBOOT_COLD)`.
 *
 * The ROM bootloader checks that bit at startup and parks in
 * USB-Serial-JTAG download mode regardless of GPIO9 (BOOT) state.
 * `esptool ... --before usb-reset` then connects the next attempt.
 *
 * After flashing succeeds, the new image's MCUboot wakes up with the
 * bit cleared by ROM (write-1-clear semantics during ROM read), so
 * subsequent boots are normal.
 *
 * Caller never returns.
 */
void download_mode_request_and_reboot(void);
