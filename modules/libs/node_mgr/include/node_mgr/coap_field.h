/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>

/**
 * coap_field — pure dispatcher for `/field` ECIES requests.
 *
 * Decrypts the inbound ECIES envelope, parses the JSON command
 * (op = "get" | "set", name, value), looks up the auto_engine field
 * registry, applies the operation, encrypts the response envelope.
 *
 * Transport-agnostic: called from both the (formerly direct) CoAP
 * `/field` listener and the new nn_proto H2D FIELD_OP handler.
 *
 * @param env_in            Null-terminated input ECIES envelope JSON.
 * @param env_out           Caller-supplied output buffer for the
 *                          response envelope JSON.
 * @param env_out_size      Size of *env_out.
 * @param env_out_len       Out: bytes written to *env_out (excludes NUL).
 *
 * @retval 0          Success — *env_out_len updated, *env_out NUL-terminated.
 * @retval -EINVAL    Malformed input (decrypt failed, unparseable JSON, etc.).
 *                    No output is written.
 * @retval -ENOMEM    Output buffer too small.
 */
/* Phase 3 sealed path: serve an ALREADY-DECRYPTED request JSON and
 * build the response PLAINTEXT (no ECIES either way — the caller wraps
 * with the session AEAD).  *resp_len out.  Returns 0 or -errno. */
int field_op_dispatch_plain(const char *plain_req,
			    char *resp_out, size_t resp_cap,
			    int *resp_len);

int field_op_dispatch(const char *env_in,
		      char *env_out, size_t env_out_size,
		      size_t *env_out_len);
