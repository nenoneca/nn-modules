/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Software-only path into ESP32-C6 ROM download mode.
 *
 * Background: the ROM bootloader's chip-mode latch is normally driven
 * by GPIO 9 strapping at reset (low → download, high → SPI boot).  But
 * the ROM also reads LP_AON_SYS_CFG_REG.FORCE_DOWNLOAD_BOOT — a bit in
 * the LP-AON (always-on) domain that survives software reset — and
 * forces download mode if set.  This lets the running firmware request
 * a reboot-into-download via a single MMIO write + Zephyr sys_reboot.
 *
 * Confirmed by Espressif's flasher_stub itself, which clears the same
 * bit after exiting:
 *   tools/flasher_stub/stub_io.c:265
 *     REG_CLR_MASK(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
 *     software_reset_cpu(0);
 *
 * For ESP32-C6 the register is `LP_AON_SYS_CFG_REG @ 0x600B1034`,
 * bit 30, defined in
 *   third_party/hal_espressif/components/soc/esp32c6/register/soc/lp_aon_reg.h
 */

#include <nn_osal/osal.h>
#include <node_mgr/download_mode.h>

NN_OSAL_LOG_MODULE(download_mode);

#define LP_AON_SYS_CFG_REG          0x600B1034U
#define LP_AON_FORCE_DOWNLOAD_BOOT  (1U << 30)

static inline uint32_t mmio_r32(uint32_t addr)
{
	return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void mmio_w32(uint32_t addr, uint32_t val)
{
	*(volatile uint32_t *)(uintptr_t)addr = val;
}

void download_mode_request_and_reboot(void)
{
	uint32_t v = mmio_r32(LP_AON_SYS_CFG_REG);
	v |= LP_AON_FORCE_DOWNLOAD_BOOT;
	mmio_w32(LP_AON_SYS_CFG_REG, v);

	NN_LOG_INF("FORCE_DOWNLOAD_BOOT set; resetting into ROM download mode");
	/* Tiny delay so the LOG line drains and any pending TCP/UART buffers
	 * have a chance to flush before we yank the chip. */
	nn_osal_sleep_ms(100);
	nn_osal_sys_reboot(NN_OSAL_REBOOT_COLD);
	/* unreachable */
}

/* ── shell ─────────────────────────────────────────────────────────────── */

static int cmd_download(nn_osal_shell_ctx_t *sh, size_t argc, char **argv)
{
	NN_OSAL_UNUSED(argc); NN_OSAL_UNUSED(argv);
	nn_osal_shell_print(sh, "Setting LP_AON FORCE_DOWNLOAD_BOOT and rebooting.");
	nn_osal_shell_print(sh, "After re-enumeration, run:");
	nn_osal_shell_print(sh, "  esptool --chip esp32c6 --port <usb-jtag> "
				"--before usb-reset write-flash ...");
	download_mode_request_and_reboot();
	return 0;  /* unreachable */
}

NN_OSAL_SHELL_CMD_REGISTER(download, cmd_download,
			   "Reboot into ROM download mode (no BOOT button needed). "
			   "Sets LP_AON.FORCE_DOWNLOAD_BOOT then sys_reboot.");
