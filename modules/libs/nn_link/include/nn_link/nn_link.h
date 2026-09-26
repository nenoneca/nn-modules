/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * nn_link — a serial-like, packet-framed data link between the P4 (master)
 * and the C6 (slave) over SDIO.
 *
 * Roles are selected at build time by the nn_registry:
 *   - CONFIG_NN_LINK_ROLE_MASTER  → P4, SDMMC host  (nn_link_master.c)
 *   - CONFIG_NN_LINK_ROLE_SLAVE   → C6, SDIO slave  (nn_link_slave.c)
 *
 * Both sides expose the same tiny API.  Each nn_link_send() is delivered to
 * the peer as one packet (SDIO preserves packet boundaries), and each
 * received packet is handed to the rx callback — so it behaves like a
 * message-oriented serial port.
 *
 * Wiring (Milestone 1):
 *   C6 slave  : CLK=19  CMD=18  D0=20 D1=21 D2=22 D3=23   (fixed IO_MUX pins)
 *   P4 master : CLK=2   CMD=3   D0=15 D1=16 D2=17 D3=18   (GPIO matrix)
 *   C6 GPIO6 -> P4 GPIO35 (BOOT strap);  C6 GPIO5 -> P4 RESET/EN  (see nn_p4ctl)
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Largest single nn_link_send()/received packet (bytes). */
#define NN_LINK_MAX_PACKET   1024

typedef void (*nn_link_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);

/* Initialise the link hardware for this node's role (does not yet bring the
 * link up).  Idempotent. */
esp_err_t nn_link_init(void);

/* Bring the link up.
 *   master: probe + enumerate the slave card, init the serial-slave link,
 *           and perform the HELLO handshake (retries until the slave answers).
 *   slave : start the SDIO slave and begin listening for the master. */
esp_err_t nn_link_start(void);

/* Register the callback invoked (from the link rx task) for every packet
 * received from the peer.  Pass NULL to clear. */
void nn_link_set_rx_cb(nn_link_rx_cb_t cb, void *ctx);

/* Send one packet to the peer.  Blocks briefly; returns ESP_ERR_INVALID_STATE
 * if the link is not connected yet. */
esp_err_t nn_link_send(const uint8_t *data, size_t len);

/* True once the link is established (master: handshake done; slave: first
 * packet received from master). */
bool nn_link_is_connected(void);

/* Block until the link is connected or `timeout_ms` elapses (<0 = forever).
 * Returns ESP_OK or ESP_ERR_TIMEOUT. */
esp_err_t nn_link_wait_connected(int timeout_ms);

/* "master" or "slave". */
const char *nn_link_role_str(void);

/* Register the `link-*` console commands (send / status / connect / wait). */
void nn_link_cli_register(void);

#ifdef __cplusplus
}
#endif
