/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

/*
 * nn_p4ctl — ESP32-C6 control of the ESP32-P4's boot/reset straps.
 *
 * The C6 networking co-processor owns the P4's power-up sequencing so it can
 * (re)boot the P4 into normal mode or into ROM-download mode (for flashing
 * the P4 over the C6 later).  Wiring:
 *
 *   C6 BOOT pin  ->  P4 GPIO35   (BOOT strap: high = normal boot, low = download)
 *   C6 RESET pin ->  P4 EN/RESET (active low: drive low to hold P4 in reset)
 *
 * Both C6 pins are push-pull outputs.  RESET idles high (P4 running); BOOT
 * idles high (normal boot).  The GPIOs come from the board profile
 * (nn_registry Kconfig, NN_BOARD_*); combo-harness defaults are BOOT=6, RESET=5.
 */
/* Fallbacks keep the header self-contained if included without sdkconfig. */
#ifndef CONFIG_NN_P4CTL_PIN_BOOT
#define CONFIG_NN_P4CTL_PIN_BOOT  6
#endif
#ifndef CONFIG_NN_P4CTL_PIN_RESET
#define CONFIG_NN_P4CTL_PIN_RESET 5
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define NN_P4CTL_PIN_BOOT   CONFIG_NN_P4CTL_PIN_BOOT   /* C6 GPIO -> P4 GPIO35  */
#define NN_P4CTL_PIN_RESET  CONFIG_NN_P4CTL_PIN_RESET  /* C6 GPIO -> P4 EN/RESET */

/* Configure the two control GPIOs (RESET=high, BOOT=high). Idempotent. */
esp_err_t nn_p4ctl_init(void);

/* Hold/release the P4 in reset (RESET line low/high). */
esp_err_t nn_p4ctl_assert_reset(void);
esp_err_t nn_p4ctl_release_reset(void);

/* Power-cycle the P4 into NORMAL boot (BOOT high, pulse RESET low->high). */
esp_err_t nn_p4ctl_power_cycle(void);

/* Reboot the P4 into ROM DOWNLOAD mode (BOOT low across the reset pulse,
 * then release BOOT high). */
esp_err_t nn_p4ctl_enter_download(void);

/* Register the `p4 ...` console commands. */
void nn_p4ctl_cli_register(void);

#ifdef __cplusplus
}
#endif
