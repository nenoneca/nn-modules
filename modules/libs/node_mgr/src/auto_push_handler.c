/* SPDX-License-Identifier: Apache-2.0 */

/*
 * auto_push_handler.c — H2D AUTO_PUSH responder.
 *
 * On an H2D AUTO_PUSH frame the sensor:
 *   1. Decrypts the ECIES envelope (plaintext = raw auto_bin blob).
 *   2. Calls auto_engine_load() to swap in the new rule set.
 *   3. Replies D2H AUTO_ACK with a 1-byte status.
 *
 * The wire format is documented in docs/protocol/nn_proto.md:
 *   H2D AUTO_PUSH inner = [cmd:2 | tid:4 | ECIES envelope JSON]
 *   D2H AUTO_ACK  inner = [cmd:2 | tid:4 | u8 status]
 *
 * status 0 = applied, non-zero = errno-style failure (e.g. -EINVAL,
 *           -ENOMEM) clipped to 1 byte.
 */

#include <nn_osal/osal.h>
#include <node_mgr/auto_engine.h>
#include <node_mgr/auto_push_handler.h>
#include <node_mgr/nn_proto_client.h>

#include <errno.h>
#include <string.h>


#include <fw_common/hub_crypto.h>
#include <nn_proto/nn_proto.h>

NN_OSAL_LOG_MODULE(auto_push);

/* Envelope JSON ≤ ~256 B; auto_bin blob ≤ 256 B typical, 512 B headroom. */
#define ENV_BUF      640
#define PLAIN_BUF    512

/* Heap-backed scratch — keeps the rx_thread stack lean.  Large
 * buffers on the stack of any handler are a hazard because the
 * handler dispatch path is shared by all H2D commands.  Allocations
 * happen only while AUTO_PUSH is being decoded and freed on every
 * exit path. */
K_HEAP_DEFINE(s_auto_push_heap, ENV_BUF + PLAIN_BUF + 256);

#define AP_HEAP_ALLOC_TIMEOUT_MS  500u

static void on_auto_push(uint32_t tid,
			 const uint8_t *body, size_t body_len,
			 void *user)
{
	ARG_UNUSED(user);

	if (body_len == 0 || body_len >= ENV_BUF) {
		NN_LOG_WRN("AUTO_PUSH body size out of range: %zu", body_len);
		uint8_t st = (uint8_t)(-EINVAL & 0xff);
		(void)nn_proto_client_send_d2h_reply(NN_PROTO_CMD_AUTO_ACK,
						     tid, &st, 1);
		return;
	}

	char    *env_in = k_heap_alloc(&s_auto_push_heap, ENV_BUF,
				       K_MSEC(AP_HEAP_ALLOC_TIMEOUT_MS));
	uint8_t *plain  = k_heap_alloc(&s_auto_push_heap, PLAIN_BUF,
				       K_MSEC(AP_HEAP_ALLOC_TIMEOUT_MS));
	if (!env_in || !plain) {
		NN_LOG_ERR("AUTO_PUSH heap alloc failed (env=%p plain=%p)",
			env_in, plain);
		if (env_in) k_heap_free(&s_auto_push_heap, env_in);
		if (plain)  k_heap_free(&s_auto_push_heap, plain);
		uint8_t st = (uint8_t)(-ENOMEM & 0xff);
		(void)nn_proto_client_send_d2h_reply(NN_PROTO_CMD_AUTO_ACK,
						     tid, &st, 1);
		return;
	}

	memcpy(env_in, body, body_len);
	env_in[body_len] = '\0';

	size_t plain_len = PLAIN_BUF;
	int rc = hub_crypto_decrypt(env_in, plain, &plain_len);
	if (rc != 0) {
		NN_LOG_WRN("AUTO_PUSH decrypt rc=%d", rc);
		uint8_t st = (uint8_t)(rc & 0xff);
		(void)nn_proto_client_send_d2h_reply(NN_PROTO_CMD_AUTO_ACK,
						     tid, &st, 1);
		goto out;
	}

	rc = auto_engine_load(plain, plain_len);
	if (rc < 0) {
		NN_LOG_WRN("AUTO_PUSH auto_engine_load rc=%d", rc);
		uint8_t st = (uint8_t)(rc & 0xff);
		(void)nn_proto_client_send_d2h_reply(NN_PROTO_CMD_AUTO_ACK,
						     tid, &st, 1);
		goto out;
	}

	NN_LOG_INF("AUTO_PUSH applied %zu B blob (rules=%d) tid=0x%08x",
		plain_len, auto_engine_rule_count(), tid);

	uint8_t st = 0;
	(void)nn_proto_client_send_d2h_reply(NN_PROTO_CMD_AUTO_ACK, tid, &st, 1);

out:
	k_heap_free(&s_auto_push_heap, env_in);
	k_heap_free(&s_auto_push_heap, plain);
}

int auto_push_handler_start(void)
{
	return nn_proto_client_register_h2d_handler(NN_PROTO_CMD_AUTO_PUSH,
						    on_auto_push, NULL);
}
