/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_pal/openthread.h — Thread mesh init + lifecycle + dataset glue.
 *
 * OpenThread itself (otXxx APIs operating on `otInstance *`) is
 * upstream-portable across Zephyr / ESP-IDF / NCS / OT-Posix.  What
 * varies between platforms is the *wrapping*:
 *   - getting hold of the otInstance singleton
 *   - hooking the radio platform layer to the chip's 802.15.4 driver
 *   - mutex around the OT API (in Zephyr this is `openthread_api_mutex_*`)
 *   - state-change callback registration
 *
 * This PAL exposes only the wrapping surface.  Code that needs to read
 * a dataset, check if commissioned, set mesh role, etc., still calls
 * `otXxx(nn_pal_ot_instance(), ...)` directly — that part of the API
 * is portable already.
 */

/* Opaque alias.  Resolves to `otInstance *` from <openthread/instance.h>
 * inside the backend; consumers only need `void *` since they pass it
 * back to OT APIs unchanged. */
typedef void *nn_pal_ot_instance_t;

/* Platform init (idempotent).  Brings up the radio + OT stack on
 * Zephyr that's normally implicit in IDF.  Returns 0 / -ENODEV /
 * -EAGAIN. */
int nn_pal_ot_init(void);

/* The OT singleton.  Returns NULL until init has succeeded. */
nn_pal_ot_instance_t nn_pal_ot_instance(void);

/* OT mutex — every otXxx call should be wrapped to be SMP-safe on
 * Zephyr.  No-ops on backends where OT is single-threaded by design. */
void nn_pal_ot_mutex_lock(void);
void nn_pal_ot_mutex_unlock(void);

/* IP6 + Thread enable shorthand.  Equivalent to:
 *   otIp6SetEnabled(inst, true);
 *   otThreadSetEnabled(inst, true);
 * but with the mutex held.  Returns 0 on success. */
int nn_pal_ot_iface_start(void);
int nn_pal_ot_iface_stop(void);

/* Whether the device has an Active Dataset committed in NVS.  Wraps
 * otDatasetIsCommissioned() with the mutex held. */
bool nn_pal_ot_is_commissioned(void);

/* State-change callback registration.  The PAL forwards otStateChangedFlags
 * notifications via this callback.  Multiple registrations allowed; the
 * backend may cap the count (Zephyr: 4). */
typedef void (*nn_pal_ot_state_cb_t)(uint32_t flags, void *user);

int nn_pal_ot_state_cb_register(nn_pal_ot_state_cb_t cb, void *user);
int nn_pal_ot_state_cb_unregister(nn_pal_ot_state_cb_t cb, void *user);

/* IPv6 scope-id of the Thread network interface — needed when sending
 * to link-local addresses (ff02::… mDNS multicast, etc.).  Returns
 * 0 if the OT iface isn't up yet. */
uint32_t nn_pal_ot_iface_scope_id(void);
