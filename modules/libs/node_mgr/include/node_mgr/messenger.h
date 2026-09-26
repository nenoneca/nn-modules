/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MESSENGER_H
#define MESSENGER_H

#include <stdint.h>
#include <stddef.h>

/**
 * Start the background UDP receive thread.
 * Call once after nm_init() succeeds.
 */
void messenger_start(void);

/**
 * Resolve hostname and send a UDP message.
 *
 * Resolution order:
 *   1. DNS cache (LRU or FIFO, configurable)
 *   2. mDNS query (zsock_getaddrinfo → CONFIG_MDNS_RESOLVER)
 *      On success the result is inserted into the cache.
 *
 * Prefers mesh-local (fd::/8) over link-local (fe80::/10) addresses
 * when multiple records are returned by mDNS.
 *
 * @param hostname  e.g. "device2.local"
 * @param message   Null-terminated string payload.
 * @return 0 on success, negative errno on failure.
 */
int messenger_send(const char *hostname, const char *message);

#endif /* MESSENGER_H */
