/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/*
 * nn_ctrl — encrypted control channel between the C6 and the hub.
 *
 * The C6 dials the provisioned hub control endpoint (hub_host:hub_port) and
 * establishes an nn_sectun secure session keyed by the hub's X25519 pubkey
 * (the same identity used for provisioning).  It then serves a simple
 * request/reply command protocol so the hub can query/command the camera node
 * over an authenticated, encrypted link (separate from the video uplink).
 *
 * Request/reply records (plaintext inside the nn_sectun session):
 *   req:  [u8 opcode][payload...]
 *   rep:  [u8 opcode][payload...]
 * Opcodes:
 *   0x01 PING    → reply 0x01 "pong"
 *   0x02 STATUS  → reply 0x02 <one-line status string>
 *   else         → reply 0xFF (unknown)
 *
 * The channel only comes up once the node is provisioned (hub endpoint + hub
 * key present); otherwise the task idles and retries.
 */

esp_err_t nn_ctrl_init(void);    /* register the `ctrl` console command */
esp_err_t nn_ctrl_start(void);   /* spawn the control-channel client task */

/* Set the hub control endpoint + X25519 pubkey directly, for hosts that don't
 * use nn_prov (e.g. the esp-hosted P4 host).  Takes precedence over any
 * nn_prov-provisioned values.  Safe to call before or after nn_ctrl_start(). */
void nn_ctrl_set_hub(const uint8_t pub[32], const char *host, uint16_t port);

/* Provide the STATUS-reply string (the hub reads it to identify/register the
 * device).  Writes into out (cap n) and returns its length.  If unset, defaults
 * to nn_prov_status_str (C6) or "status=up". */
typedef size_t (*nn_ctrl_status_fn_t)(char *out, size_t n);
void nn_ctrl_set_status_fn(nn_ctrl_status_fn_t fn);
