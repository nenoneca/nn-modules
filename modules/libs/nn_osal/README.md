# `nn_osal` — OS Abstraction Layer

Sole entry point for nn libraries to access RTOS / platform services.
No nn library should `#include <zephyr/...>`, `<FreeRTOS.h>`, or call
libc functions like `memcpy()` directly — go through `nn_osal/*`
instead.

## Why

The project is rapidly outgrowing its tight Zephyr coupling. Three
near-term reasons to decouple:

1. **Host tooling** — fuzzers, simulators, replay tools for nn_proto
   would compile faster + run easier under POSIX than as full Zephyr
   builds.
2. **Vendor portability** — a future ASIC or non-Zephyr MCU should not
   require re-writing `node_mgr/*`.
3. **Static analysis + test coverage** — easier to mock a small
   `nn_osal_*` surface than the full Zephyr kernel API.

For v1 we keep only the Zephyr backend; the OSAL is just a wall.
Future backends slot into `src/<name>/`.

## Module layout

```
nn_osal/
├── include/nn_osal/
│   ├── osal.h        umbrella (includes all the below)
│   ├── thread.h      threads, K_THREAD_STACK_DEFINE equivalent
│   ├── sync.h        semaphores, mutexes
│   ├── work.h        work items, delayable work, dedicated queues
│   ├── time.h        uptime_ms, sleep_ms, busy_wait_us
│   ├── gpio.h        digital GPIO + edge IRQs
│   ├── log.h         printf-style logging (LOG_INF/WRN/ERR/DBG)
│   ├── storage.h     KV store + DFU image staging
│   ├── system.h      reboot, panic, app_version, init hooks
│   ├── socket.h      BSD-style v6 sockets
│   ├── libc.h        memcpy/memset/snprintf/... wrappers
│   └── internal/     backend-specific type definitions
└── src/
    └── zephyr/       (only backend today)
```

## How to use

In a library that wants OSAL:

```c
#include <nn_osal/osal.h>
NN_OSAL_LOG_MODULE("my_mod");

NN_OSAL_THREAD_STACK_DEFINE(stack, 2048);
static nn_osal_thread_t thr;

static void entry(void *a, void *b, void *c) {
    NN_LOG_INF("hi");
    while (1) nn_osal_sleep_ms(1000);
}

int my_init(void) {
    return nn_osal_thread_create(&thr, stack, sizeof stack,
                                 entry, NULL, NULL, NULL,
                                 5, "my-thr");
}
```

That's the whole API surface. No Zephyr headers needed.

## What's intentionally NOT in OSAL

| Concern | Lives where |
|---|---|
| CoAP, OpenThread, BLE protocol stacks | `node_mgr/` adapters (not OS APIs) |
| Crypto (X25519, AES-GCM, P-256) | `fw_common/hub_crypto`, PSA via separate path |
| Vendor radio drivers | apps' DT overlays |
| App-specific configuration | per-app `Kconfig` + `prj.conf` |
| Wall-clock time | `node_mgr/time_sync` (network-derived) |

## Migration status

Phase 1 — Define API + Zephyr backend.  **Done.**
Phase 2 — Migrate `time_sync` + `button_*`.
Phase 3 — Migrate rest of node_mgr.
Phase 4 — Migrate fw_common, collapse its `platform/{zephyr,linux}/`.
Phase 5 — CI lint forbidding `#include <zephyr/...>` outside apps.

## Adding a new backend

1. `mkdir src/<name>`
2. Implement each `nn_osal_*` function from the public headers
3. Add backend-specific opaque struct defs in
   `include/nn_osal/internal/backend_<category>.h` inside an
   `#ifdef CONFIG_NN_OSAL_BACKEND_<NAME>` block
4. Add the Kconfig choice option
5. Wire the source file list into the top-level `CMakeLists.txt`
6. Write a smoke test app under `apps/osal_smoke_<name>/`

The Zephyr backend is ~700 LOC; a POSIX backend would be similar.
