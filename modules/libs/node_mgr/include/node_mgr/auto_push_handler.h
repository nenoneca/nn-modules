/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/**
 * auto_push_handler — H2D AUTO_PUSH responder.
 *
 * Call once at app init AFTER hub_crypto + auto_engine + nn_proto_client
 * are initialised.  Registers an H2D handler for cmd 0x0026 that
 * ECIES-decrypts the inbound blob, applies it via auto_engine_load(),
 * and replies D2H AUTO_ACK (0x0027) with a 1-byte status.
 *
 * Returns 0 on success, negative errno if registration fails (e.g.
 * the H2D handler table is full).
 */
int auto_push_handler_start(void);
