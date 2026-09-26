/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/*
 * nn_osal — OS Abstraction Layer for the nn project.
 *
 * Sole entry point for nn libraries (node_mgr, fw_common, nn_proto) to
 * access RTOS / platform services.  No nn lib should `#include
 * <zephyr/...>`, `#include <FreeRTOS.h>`, or call libc functions like
 * memcpy() directly — go through nn_osal instead.
 *
 * Apps (under device/apps/) are still allowed to use platform APIs
 * directly; they are platform-specific by definition.  Libraries are
 * the portable layer.
 *
 * Currently the only backend is Zephyr (under src/zephyr/).  Adding a
 * new backend means implementing the headers under a new
 * src/<target>/ tree and selecting it via CONFIG_NN_OSAL_BACKEND_*.
 *
 * Category coverage:
 *   1. Threads, semaphores, mutexes        → thread.h / sync.h
 *   2. Deferred work / work queues          → work.h
 *   3. Time, sleep, monotonic clock         → time.h
 *   4. GPIO (digital in / out + irq)        → gpio.h
 *   5. Logging                              → log.h
 *   6. Persistent key-value storage + DFU   → storage.h
 *   7. System control (reboot, init hooks)  → system.h
 *   8. BSD-style sockets                    → socket.h
 *   9. C standard library wrappers          → libc.h
 *
 * Domain protocol stacks (CoAP, OpenThread, BLE) are NOT abstracted;
 * they live in node_mgr's higher-level adapters.
 */

#include "thread.h"
#include "sync.h"
#include "work.h"
#include "time.h"
#include "gpio.h"
#include "log.h"
#include "storage.h"
#include "system.h"
#include "socket.h"
#include "libc.h"
/* P6b additions */
#include "uart.h"
#include "watchdog.h"
#include "shell.h"
/* P8 additions — tiny helpers that previously dragged in
 * <zephyr/sys/byteorder.h> + <zephyr/sys/util.h>. */
#include "byteorder.h"
#include "util.h"
