/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * field_relay — Phase 6 hub <-> device field-op relay (H2D dispatcher).
 *
 * Decodes an inbound FIELD_OP frame received from the hub via the
 * gateway, runs the shared `field_op_dispatch` (ECIES decrypt ->
 * auto_engine set/get_field -> ECIES-encrypt the reply), and sends
 * back a FIELD_REPLY D2H frame on the same tid.
 *
 * Inner-cmd layout (must stay in sync with hub/proto.py):
 *   [cmd:2 LE | tid:4 LE | ECIES envelope JSON, NUL-terminated]
 *
 * Apps register an `on_h2d` callback with `nn_proto_client`; for the
 * common case of "this device wants the standard hub-driven field
 * R/W", the on_h2d can just delegate to this helper:
 *
 *     static void on_h2d(const uint8_t *payload, size_t len, void *user)
 *     {
 *         if (field_relay_try_handle_h2d(payload, len)) {
 *             return;
 *         }
 *         // ...app-specific cmd handling here...
 *     }
 *
 * Returns true if the frame was a FIELD_OP and was consumed (whether
 * the dispatch succeeded or any failure was already logged); returns
 * false otherwise so the caller can fall through to its own dispatch.
 */
bool field_relay_try_handle_h2d(const uint8_t *payload, size_t len);
