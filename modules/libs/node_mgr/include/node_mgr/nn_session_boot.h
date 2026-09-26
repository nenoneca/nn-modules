/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_session_boot — device side of the Phase-2 symmetric-session
 * handshake.  At boot the device mints a random 8-byte session salt and
 * advertises it (hex) in its INFO_REPLY; the hub replies with SESS_INIT
 * carrying its own salt; both derive a fresh AES-GCM session from the
 * static X25519 ECDH.  A one-shot SESS_PROBE (a sealed known plaintext)
 * lets the hub confirm key agreement over the real mesh.
 */

/* Generate this boot's session salt and register the SESS_INIT handler.
 * Call once after hub_crypto + nn_proto_client are up. */
int nn_session_boot_init(void);

/* Write this boot's salt as 16 lowercase hex chars + NUL into *out
 * (>=17 bytes).  Used by info_handler to add "sess" to INFO_REPLY.
 * Returns 0, or -1 if not initialised. */
int nn_session_boot_salt_hex(char out[17]);

/* Raw 8-byte salt (for the heartbeat piggyback).  -1 if not minted. */
int nn_session_boot_salt_raw(uint8_t out[8]);

/* True once SESS_INIT completed and the session is usable. */
bool nn_session_boot_ready(void);

/* Announce this boot's salt via a small SESS_HELLO frame if the session
 * is not yet established.  Cheap no-op afterwards.  Call periodically
 * from an existing worker (heartbeat). */
void nn_session_boot_hello_tick(void);

/* Thread-safe seal/open on this boot's hub session (mutex-guarded —
 * callers run on field_relay WQ while SESS_INIT derives on rx path).
 * Same contracts as nn_session_seal/_open; -ENOTCONN if no session. */
int nn_session_boot_seal(const uint8_t *aad, size_t aad_len,
			 const uint8_t *pt, size_t pt_len,
			 uint8_t *out, size_t out_cap, size_t *out_len);
int nn_session_boot_open(const uint8_t *aad, size_t aad_len,
			 const uint8_t *in, size_t in_len,
			 uint8_t *out, size_t out_cap, size_t *out_len);
