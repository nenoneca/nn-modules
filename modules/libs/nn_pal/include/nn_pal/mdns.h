/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_pal/mdns.h — service advertisement.
 *
 * The nn project uses mDNS for:
 *   - sensor → hub discovery (the sensor publishes `_nn-device._udp.local`
 *     so the hub can find it without a static IP)
 *   - hub → gateway discovery (gateway publishes `_nn-gw._tcp.local`)
 *   - operator → hub discovery (`_nn-hub._tcp.local`)
 *
 * The API is intentionally tiny:
 *   - set hostname (so `<host>.local` resolves)
 *   - publish a service (one call per service; subsequent calls add)
 *   - optional client-side resolve / browse (rarely used inside nn —
 *     gateway discovery on the sensor side uses GATEWAY_HELLO multicast
 *     instead).
 */

/* Set the device's mDNS hostname.  Resolves to `<host>.local` on the
 * mesh / LAN.  Call once at boot; safe to call again on rename. */
int nn_pal_mdns_set_hostname(const char *host);

/* Single TXT record entry. */
typedef struct {
    const char *key;   /* no '=' or whitespace */
    const char *val;   /* may be NULL or "" for key-only entries */
} nn_pal_mdns_txt_t;

/* Publish a service.  Returns an opaque handle (>0) on success or
 * negative errno.  Handle is needed to unpublish later. */
typedef int nn_pal_mdns_service_t;

int nn_pal_mdns_advertise(const char *service_type,   /* e.g. "_nn-device._udp" */
                          uint16_t    port,
                          const nn_pal_mdns_txt_t *txt,
                          size_t txt_count,
                          nn_pal_mdns_service_t *out_handle);

int nn_pal_mdns_unpublish(nn_pal_mdns_service_t handle);

/* Lifecycle (mostly invoked once at boot by the platform glue). */
int  nn_pal_mdns_init(void);
void nn_pal_mdns_shutdown(void);
