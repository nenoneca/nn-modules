/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/*
 * nn_pal — Platform Abstraction Layer for protocol subsystems.
 *
 * Sister to nn_osal:
 *   - nn_osal wraps OS primitives (threads, sync, time, GPIO, log, sockets).
 *     50-250 LOC per backend; stable across decades.
 *   - nn_pal wraps protocol subsystems (BLE, DFU, OpenThread, WiFi, mDNS).
 *     80-800 LOC per backend; vendor-rev'd more often.
 *
 * Apps + libs should NEVER #include <zephyr/bluetooth/...>,
 * <zephyr/dfu/...>, <zephyr/net/openthread.h>, <zephyr/net/wifi*>,
 * <zephyr/net/dns_sd.h> directly.  Use nn_pal/<surface>.h instead;
 * the backend translates to the platform's native API.
 *
 * Surfaces:
 *   1. ble.h        — Bluetooth GATT (peripheral + central)
 *   2. dfu.h        — Firmware DFU (extends nn_osal/storage.h DFU section)
 *   3. openthread.h — Thread mesh init / dataset / role
 *   4. wifi.h       — STA bring-up + event callback
 *   5. mdns.h       — Service advertisement
 *
 * Backends today: Zephyr only.  Adding a new backend (NimBLE on
 * ESP-IDF, POSIX simulator, ...) means writing src/<backend>/*.c and
 * selecting it via CONFIG_NN_PAL_BACKEND_*.
 */

#include "ble.h"
#include "dfu.h"
#include "openthread.h"
#include "wifi.h"
#include "mdns.h"
