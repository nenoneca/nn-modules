#!/usr/bin/env python3
"""
lint_no_zephyr.py — enforce the nn_osal wall around Zephyr.

Run from anywhere; defaults to scanning the nn modules tree.  Exits
non-zero if any lib (.c/.h) file under modules/libs/{node_mgr,fw_common,
nn_proto,nn_proto_identity} includes a Zephyr header from the BANNED
list.  Apps (device/apps/*) are unaffected — they're platform-specific
by definition and may use Zephyr directly.

Banned headers (must go through nn_osal):
    <zephyr/kernel.h>
    <zephyr/logging/log.h>
    <zephyr/sys/reboot.h>

Allowed headers (OSAL doesn't cover these yet — protocol/driver libs):
    <zephyr/net/coap.h>           — protocol library
    <zephyr/net/openthread.h>     — protocol library
    <zephyr/net/net_if.h>         — net stack
    <zephyr/net/net_ip.h>         — net stack
    <zephyr/net/hostname.h>       — net stack
    <zephyr/net/socket.h>         — partial nn_osal/socket.h, full migration TODO
    <zephyr/bluetooth/*>          — vendor BLE stack
    <zephyr/drivers/gpio.h>       — OSAL GPIO incomplete
    <zephyr/devicetree.h>         — DT macros only used at compile time
    <zephyr/dfu/*>                — OTA-specific, gated behind CONFIG_BOOTLOADER_MCUBOOT
    <zephyr/storage/flash_map.h>  — OTA-specific
    <zephyr/settings/settings.h>  — partial nn_osal/storage.h; migrating
    <zephyr/sys/byteorder.h>      — host/network byte-order helpers
    <zephyr/sys/util.h>           — ARRAY_SIZE, MAX, MIN macros
    <zephyr/init.h>               — SYS_INIT priority macros
    <zephyr/shell/shell.h>        — debug shell only
    <zephyr/app_version.h>        — generated from VERSION file
    <zephyr/sys/printk.h>         — printk fallback (panic path)

To enforce a tighter wall later, move headers from the allowed list
into BANNED.

Usage:
    python3 lint_no_zephyr.py [--root PATH]    # exits 0 / 1
"""
import argparse
import re
import sys
from pathlib import Path

BANNED = {
    # P3 / P6a — kernel + log + reboot
    'zephyr/kernel.h',
    'zephyr/logging/log.h',
    'zephyr/sys/reboot.h',
    # P6b/P6c — wrapped behind nn_osal/{gpio,uart,watchdog}.h
    'zephyr/drivers/gpio.h',
    'zephyr/drivers/uart.h',
    'zephyr/task_wdt/task_wdt.h',
    # P7b — wrapped behind nn_pal/mdns.h
    'zephyr/net/dns_sd.h',
    'zephyr/net/hostname.h',
    # P7c — wrapped behind nn_pal/openthread.h
    'zephyr/net/openthread.h',
    # P7d — wrapped behind nn_pal/wifi.h
    'zephyr/net/wifi.h',
    'zephyr/net/wifi_mgmt.h',
    # P7e — wrapped behind nn_pal/dfu.h
    'zephyr/dfu/mcuboot.h',
    'zephyr/dfu/flash_img.h',
    'zephyr/storage/flash_map.h',
    # P7f — wrapped behind nn_pal/ble.h (peripheral + central GATT)
    'zephyr/bluetooth/bluetooth.h',
    'zephyr/bluetooth/conn.h',
    'zephyr/bluetooth/gatt.h',
    'zephyr/bluetooth/uuid.h',
    # P8 — wrapped behind nn_osal/{byteorder,util,shell}.h + nn_osal_kv
    'zephyr/sys/byteorder.h',
    'zephyr/sys/util.h',
    'zephyr/shell/shell.h',
    'zephyr/settings/settings.h',
    # P9 — wrapped behind nn_osal/{system,gpio}.h + nn_pal/log_sink.h
    'zephyr/init.h',           # NN_OSAL_INIT / NN_OSAL_INIT_EARLY
    'zephyr/device.h',         # NN_OSAL_GPIO_DT_* + nn_osal_gpio_port_ready
    'zephyr/devicetree.h',     # NN_OSAL_GPIO_DT_* macros
    'zephyr/app_version.h',    # nn_osal_app_version()
    'zephyr/logging/log_backend.h',     # nn_pal_log_sink_*
    'zephyr/logging/log_backend_std.h',
    'zephyr/logging/log_core.h',
    'zephyr/logging/log_ctrl.h',
    'zephyr/logging/log_output.h',
    # P9 — auto_engine moved to nn_proto D2D, deletes intra-mesh CoAP
    'zephyr/net/coap.h',
    # P9 — net stack fully wrapped behind nn_osal/socket.h
    'zephyr/net/socket.h',
    'zephyr/net/net_ip.h',
    'zephyr/net/net_if.h',
}

# Library names scanned.  Resolved against `--root` by trying both
# `<root>/libs/<name>` (nn-modules repo layout) and
# `<root>/modules/libs/<name>` (local device tree layout).
# nn_osal/src/zephyr/ IS allowed to include zephyr — that's the backend.
SCAN_LIBS = [
    'node_mgr',
    'fw_common',
    'nn_proto',
    'nn_proto_identity',
]

# Application directories scanned with `--scope=apps` (P6a).  Same
# BANNED set as libs — kernel.h, logging/log.h, sys/reboot.h have nn_osal
# equivalents and apps must use them.  The bigger Zephyr surfaces apps
# also use (drivers/gpio, drivers/uart, devicetree, dfu/mcuboot, BLE,
# net/openthread, shell/shell, …) are still allowed — those need
# P6b–P6d to wrap, and the lint will pick them up automatically as
# BANNED grows.
SCAN_APPS = [
    'mdns_ot_esp32c6',
    'ncp_esp32c6',
    'ncp_host_esp32c6',
]

INCLUDE_RE = re.compile(r'^\s*#include\s*<\s*(zephyr/[^>]+?)\s*>', re.M)

# Files explicitly exempted from the lint.  fw_common/log.h is a
# cross-platform shim that intentionally branches on __ZEPHYR__ to
# either include <zephyr/logging/log.h> or fall back to stderr printf
# on POSIX — it is the OSAL-style abstraction for fw_common itself, and
# removing the Zephyr include would break the gw_linux build.
WHITELIST = {
    'fw_common/include/fw_common/log.h',
    # kvstore.c is the Zephyr backend for nn_osal_kv — by design it
    # includes <zephyr/settings/settings.h>.  Lives under fw_common
    # rather than nn_osal/src/zephyr/ for historical reasons (kvstore
    # predates nn_osal).
    'fw_common/src/platform/zephyr/kvstore.c',
}


def scan_file(path: Path) -> list[str]:
    """Returns the list of banned headers found in `path`."""
    text = path.read_text(errors='replace')
    hits = []
    for m in INCLUDE_RE.finditer(text):
        h = m.group(1)
        if h in BANNED:
            hits.append(h)
    return hits


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', type=Path,
                    default=Path(__file__).resolve().parents[3],
                    help='nn modules repo root (default: auto-detect)')
    ap.add_argument('--scope', choices=('libs', 'apps', 'all'),
                    default='libs',
                    help="What to scan. 'libs' (default) = nn-owned "
                         "libraries; 'apps' = the production apps under "
                         "the local device tree; 'all' = both.")
    ap.add_argument('--device-root', type=Path, default=None,
                    help='Path to the device repo root (containing apps/). '
                         'Required when --scope includes apps; auto-tries '
                         '<root>/.. and <root>/device.')
    args = ap.parse_args()

    violations: list[tuple[Path, list[str]]] = []
    scanned = 0
    if args.scope in ('libs', 'all'):
        for lib in SCAN_LIBS:
            # Prefer <root>/libs/<lib>; fall back to <root>/modules/libs/<lib>.
            candidates = [args.root / 'libs' / lib,
                          args.root / 'modules' / 'libs' / lib]
            root = next((c for c in candidates if c.exists()), None)
            if root is None:
                print(f"[skip] lib/{lib} (not present under {args.root})")
                continue
            scanned += 1
            for ext in ('*.c', '*.h'):
                for p in root.rglob(ext):
                    rel_to_lib = p.relative_to(root.parent)
                    if str(rel_to_lib) in WHITELIST:
                        continue
                    hits = scan_file(p)
                    if hits:
                        violations.append((p, hits))

    if args.scope in ('apps', 'all'):
        # Try a few candidate locations for the apps/ tree.
        dev_root = args.device_root
        if dev_root is None:
            for c in [args.root, args.root / 'device', args.root.parent,
                      args.root.parent / 'device']:
                if (c / 'apps').exists():
                    dev_root = c
                    break
        if dev_root is None:
            print(f"[warn] apps scope requested but --device-root not found")
        else:
            for app in SCAN_APPS:
                app_root = dev_root / 'apps' / app / 'src'
                if not app_root.exists():
                    print(f"[skip] app/{app} (not under {dev_root}/apps)")
                    continue
                scanned += 1
                for ext in ('*.c', '*.h'):
                    for p in app_root.rglob(ext):
                        hits = scan_file(p)
                        if hits:
                            violations.append((p, hits))

    if not violations:
        print(f"[OK] no banned Zephyr includes in {scanned} module trees")
        return 0

    print(f"[FAIL] {len(violations)} files still include banned Zephyr headers:")
    for path, hits in violations:
        rel = path.relative_to(args.root)
        for h in sorted(set(hits)):
            print(f"  {rel}: <{h}>")
    print()
    print(f"  Fix: replace with the matching nn_osal include "
          f"(see modules/libs/nn_osal/README.md).")
    return 1

# Linux through nn_osal/nn_pal") HOLDS for TIER_PORTABLE, holds structurally
# for TIER_BACKENDS (per-platform code confined to src/<backend>/ or
# src/platform/<backend>/), and is BROKEN for TIER_ESP_ONLY — every lib in
# that list was written ESP-IDF-native during the media/camera effort with
# unguarded esp_*/freertos/driver includes in its core sources.
#
# Of the broken set: nn_camera, nn_link, nn_audio and nn_p4ctl are bound to
# specific silicon (P4 ISP/encoder, SDIO/SPI slave, I2S codec) — for them
# "portable" can only ever mean a portable API over backend dirs.  The five
# LOGIC libs (nn_netstream, nn_ctrl, nn_prov, nn_ota, nn_timesync) have no
# such excuse: everything they use (tasks, storage, timers, console, sockets)
# has an nn_osal equivalent, and they are the real migration backlog.
TIER_PORTABLE = ['detools', 'nn_crypto', 'nn_proto', 'nn_proto_identity',
                 'nn_registry', 'nn_sectun', 'nn_timesync']
TIER_BACKENDS = ['nn_osal', 'nn_pal', 'fw_common',
                 'nn_infer']  # portable core + src/pal/<name>/ backends
TIER_ESP_ONLY = ['nn_camera', 'nn_link', 'nn_audio', 'nn_p4ctl',           # silicon-bound
                 'node_mgr_media',
                 'nn_netstream', 'nn_ctrl', 'nn_prov', 'nn_ota']  # migration backlog
# node_mgr: DESIGNED Zephyr-on-ESP32 exception — its radio arbitration calls
# esp_phy/esp_ieee802154 in the body, not just the includes; treat as a
# platform-bound sensor lib, not a portability defect.


def lint_portable_tier(root):
    """FAIL if a TIER_PORTABLE lib includes any platform header outside a
    declared backend dir — this is what lets the tier claim stay true."""
    import re, os
    pat = re.compile(r'#\s*include\s*[<"](freertos/|esp_|soc/|driver/|hal/|zephyr/|nvs|lwip/)')
    bad = []
    for lib in TIER_PORTABLE:
        libdir = os.path.join(root, lib)
        for dirpath, _, files in os.walk(libdir):
            if any(seg in dirpath for seg in ("/src/esp_idf", "/src/zephyr",
                                              "/src/posix", "/src/linux",
                                              "/src/platform/", "/internal")):
                continue
            for fn in files:
                if not fn.endswith((".c", ".h")):
                    continue
                p = os.path.join(dirpath, fn)
                for i, line in enumerate(open(p, errors="replace"), 1):
                    if pat.search(line):
                        bad.append("%s:%d: %s" % (p, i, line.strip()))
    return bad

def _portable_tier_main():
    import os, sys
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    bad = lint_portable_tier(os.path.normpath(root))
    for b in bad:
        print("PORTABILITY VIOLATION:", b)
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    import sys as _s
    if '--portable-tier' in _s.argv:
        _portable_tier_main()
    sys.exit(main())

# ── Portability tiers (audit of 2026-08-05) ─────────────────────────────────
# The platform-independence rule ("every lib supports ESP-IDF, Zephyr and
