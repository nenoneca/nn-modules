/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/system.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

/* Generated unconditionally by Zephyr's build system for every app.
 * Provides APP_VERSION_STRING / APP_VERSION_EXTENDED_STRING.  There
 * is no Kconfig guard for it — the file always exists, so the previous
 * #ifdef CONFIG_APP_VERSION wrapper just hid the include and made
 * nn_osal_app_version() always return NULL. */
#include <zephyr/app_version.h>

#if defined(CONFIG_SOC_SERIES_ESP32C6)
/* Direct ROM reset entrypoint.  Provided by the Espressif HAL but not
 * declared in any Zephyr header we already include.  Declare it here
 * so we can call it without dragging in the full esp_rom_sys.h tree
 * (which pulls IDF-style headers that conflict with Zephyr at this
 * layer of the build).  Same symbol the patched
 * soc/espressif/esp32c6/soc.c::sys_arch_reboot() calls. */
extern void esp_rom_software_reset_system(void) __attribute__((noreturn));
#endif

void nn_osal_sys_reboot(nn_osal_reboot_type_t type)
{
    (void)type;
#if defined(CONFIG_SOC_SERIES_ESP32C6)
    /* Bypass Zephyr's sys_reboot() wrapper on ESP32-C6.  That wrapper
     * does irq_lock + sys_cache_data_disable + sys_cache_instr_disable
     * + sys_clock_disable BEFORE calling sys_arch_reboot.  Observed
     * 2026-06-10: when called from the OTA-apply path immediately
     * after `boot_request_upgrade()` writes the slot1 trailer, one of
     * the cache-disable steps silently hangs/crashes the calling
     * thread — other threads keep running on this single-core part,
     * the chip never resets, and the OTA appears stuck in 'applying'
     * indefinitely until an operator force-reboots over UART.
     *
     * `esp_rom_software_reset_system()` does the same digital-core
     * reset our patched sys_arch_reboot() calls (RESET_REASON_CORE_SW,
     * 0x03) so cache/MMU/MSPI state is wiped on the way out — no
     * software flush is required before invoking it. */
    esp_rom_software_reset_system();
#else
    sys_reboot(type == NN_OSAL_REBOOT_COLD ? SYS_REBOOT_COLD : SYS_REBOOT_WARM);
#endif
    CODE_UNREACHABLE;
}

void nn_osal_sys_panic(const char *reason)
{
    (void)reason;
    k_oops();
    CODE_UNREACHABLE;
}

const char *nn_osal_app_version(void)
{
    /* Prefer the EXTENDED form ("2.4.0-dev+0") which includes the tweak
     * counter — useful for distinguishing dirty / unstaged builds. */
    return APP_VERSION_EXTENDED_STRING;
}
