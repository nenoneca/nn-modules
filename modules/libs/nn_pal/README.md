# `nn_pal` — Platform Abstraction Layer

Sister library to `nn_osal`.  Same wall pattern, different scope:

| Layer | Owns | Stability |
|---|---|---|
| `nn_osal` | OS primitives (threads, sync, time, GPIO, sockets, log, libc) | Decades |
| **`nn_pal`** | **Protocol subsystems (BLE, DFU, OpenThread, WiFi, mDNS)** | **Vendor-rev'd more often** |

Apps + libs should NEVER `#include <zephyr/bluetooth/...>`,
`<zephyr/dfu/...>`, `<zephyr/net/openthread.h>`, `<zephyr/net/wifi*>`, or
`<zephyr/net/dns_sd.h>` directly.  Use `nn_pal/<surface>.h` instead.

## Surfaces

| Header | Wraps | Typical client |
|---|---|---|
| `nn_pal/ble.h` | BLE GATT (peripheral + central) | provision_peripheral.c, provision_central.c, gw_ble_prov_backend.c |
| `nn_pal/dfu.h` | Firmware DFU (MCUboot / ESP-IDF OTA) | ota_client.c, gw_ota.c, ncp_ota_client.c |
| `nn_pal/openthread.h` | Thread mesh init / dataset / role | network_manager.c, ota_client.c, auto_engine.c, info_handler.c |
| `nn_pal/wifi.h` | STA bring-up + events | ncp_host main.c |
| `nn_pal/mdns.h` | Service advertisement | mdns_ot main.c, ncp_host main.c |

## Why these and not others?

See `docs/P7_scope.md` for the full reasoning.  Quick version:
- **CoAP**: deprecated to `nn_proto`; don't abstract a dying layer.
- **net_if / net_pkt / net_l2 / dummy_iface**: Zephyr-isms heavy; the
  abstraction would leak immediately.  Kept as platform-specific app
  glue.
- **Shell**: already in `nn_osal/shell.h` (P6b).
- **Devicetree**: compile-time meta — can't be wrapped by a function-
  based PAL.  Replaced by per-board `nn_board.c` registries.

## Backend layout

```
nn_pal/
├── include/nn_pal/
│   ├── pal.h           umbrella
│   ├── ble.h
│   ├── dfu.h
│   ├── openthread.h
│   ├── wifi.h
│   └── mdns.h
└── src/
    └── zephyr/         (only backend today)
        ├── ble.c
        ├── dfu.c
        ├── openthread.c
        ├── wifi.c
        └── mdns.c
```

Adding a new backend (NimBLE on ESP-IDF, POSIX simulator, …): create
`src/<name>/*.c` files implementing each public function, add the
Kconfig choice option, wire the source list in `CMakeLists.txt`.

## Phases

- **P7a** (this commit): All 5 PAL public headers + lib scaffold + Kconfig + scope doc.  No backend impl; no app migration.
- **P7b** through **P7f**: per-PAL backend impl + app migration, smallest-first (mDNS → WiFi → OT → DFU → BLE).
- **P7g**: lint expansion bans the wrapped Zephyr headers in apps + libs.
- **P7h**: E2E validation.

See `docs/P7_scope.md`.
