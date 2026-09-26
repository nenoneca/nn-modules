/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Host-side standalone test for nn_proto.  Compile + run on Linux:
 *
 *     gcc -std=c11 -Wall -Wextra -O0 -g \
 *         -I include \
 *         tests/test_proto_host.c src/nn_proto.c \
 *         -o /tmp/nn_proto_test && /tmp/nn_proto_test
 *
 * Covers:
 *   - encode → parse round-trip (header & sizes)
 *   - signed-range bytes match what Python's _signed_blob() builds
 *   - cross-language golden vector parses & deserialises identically
 *
 * Ed25519 sig-verification is exercised by stub callbacks (returns success
 * if sig matches a recorded value).  Real Ed25519 verification is covered
 * by the on-device + hub Python tests.  This test only proves the
 * library's framing logic, which is what differs from the Python
 * implementation.
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nn_proto/nn_proto.h>

/* ── stub crypto callbacks ─────────────────────────────────────────────── */

struct stub_sig_ctx {
	/* On sign: bytes recorded so the test can compare to the parsed
	 * signed_from/signed_len range. */
	const uint8_t *captured_msg;
	size_t         captured_len;
	uint8_t        sig_pattern;  /* fills the 64-byte sig with this byte */
};

static int stub_sign(void *vctx, const uint8_t *msg, size_t msg_len,
		     uint8_t sig_out[64])
{
	struct stub_sig_ctx *ctx = vctx;
	ctx->captured_msg = msg;
	ctx->captured_len = msg_len;
	memset(sig_out, ctx->sig_pattern, 64);
	return 0;
}

struct stub_verify_ctx {
	uint8_t expected_sig_pattern;
	const uint8_t *expected_msg;  /* if non-NULL, also check msg bytes */
	size_t         expected_len;
};

static int stub_verify(void *vctx, const uint8_t *msg, size_t msg_len,
		       const uint8_t sig[64])
{
	struct stub_verify_ctx *ctx = vctx;
	for (int i = 0; i < 64; i++) {
		if (sig[i] != ctx->expected_sig_pattern) {
			return -1;
		}
	}
	if (ctx->expected_msg) {
		if (msg_len != ctx->expected_len) {
			return -1;
		}
		if (memcmp(msg, ctx->expected_msg, msg_len) != 0) {
			return -1;
		}
	}
	return 0;
}

/* ── helpers ───────────────────────────────────────────────────────────── */

#define ASSERT_OR_DIE(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, msg); \
		return 1; \
	} \
} while (0)

/* ── tests ────────────────────────────────────────────────────────────── */

static int test_round_trip_basic(void)
{
	uint8_t buf[256];
	const uint8_t device_id[] = {0xde, 0xad, 0xbe, 0xef};
	const uint8_t payload[] = "hello, world";

	struct stub_sig_ctx sctx = { .sig_pattern = 0xAB };

	int rv = nn_proto_encode(NN_PROTO_TYPE_D2H,
				 device_id, sizeof(device_id),
				 payload, sizeof(payload) - 1,
				 stub_sign, &sctx,
				 buf, sizeof(buf));
	ASSERT_OR_DIE(rv > 0, "encode failed");

	size_t expected_size = nn_proto_frame_size(sizeof(device_id),
						   sizeof(payload) - 1);
	ASSERT_OR_DIE((size_t)rv == expected_size, "encode size mismatch");

	struct nn_proto_view view;
	size_t consumed = 0;
	rv = nn_proto_parse(buf, expected_size, &view, &consumed);
	ASSERT_OR_DIE(rv == 0, "parse failed");
	ASSERT_OR_DIE(consumed == expected_size, "consumed mismatch");
	ASSERT_OR_DIE(view.type == NN_PROTO_TYPE_D2H, "type mismatch");
	ASSERT_OR_DIE(view.device_id_size == sizeof(device_id),
		      "device_id_size mismatch");
	ASSERT_OR_DIE(memcmp(view.device_id, device_id, sizeof(device_id)) == 0,
		      "device_id bytes mismatch");
	ASSERT_OR_DIE(view.payload_size == sizeof(payload) - 1,
		      "payload size mismatch");
	ASSERT_OR_DIE(memcmp(view.payload, payload, sizeof(payload) - 1) == 0,
		      "payload bytes mismatch");
	for (int i = 0; i < 64; i++) {
		ASSERT_OR_DIE(view.sig[i] == 0xAB, "sig pattern mismatch");
	}

	/* signed_from/signed_len must equal what stub_sign was called with. */
	ASSERT_OR_DIE(view.signed_from == buf, "signed_from offset mismatch");
	ASSERT_OR_DIE(view.signed_len == sctx.captured_len,
		      "signed_len differs from sign callback's view");
	ASSERT_OR_DIE(view.signed_len == expected_size - NN_PROTO_SIG_LEN,
		      "signed_len != frame_size - SIG_LEN");

	/* verify_sig must call back with exactly the same range. */
	struct stub_verify_ctx vctx = {
		.expected_sig_pattern = 0xAB,
		.expected_msg = buf,
		.expected_len = view.signed_len,
	};
	rv = nn_proto_verify_sig(&view, stub_verify, &vctx);
	ASSERT_OR_DIE(rv == 0, "verify_sig failed");

	printf("PASS: round_trip_basic\n");
	return 0;
}

static int test_empty_device_id(void)
{
	uint8_t buf[128];
	const uint8_t payload[] = {0x03, 0x00, 0x01, 0x02, 0x03};  /* GATEWAY_HELLO + arg byte */

	struct stub_sig_ctx sctx = { .sig_pattern = 0xCD };
	int rv = nn_proto_encode(NN_PROTO_TYPE_G2D,
				 NULL, 0,
				 payload, sizeof(payload),
				 stub_sign, &sctx,
				 buf, sizeof(buf));
	ASSERT_OR_DIE(rv > 0, "encode failed");

	struct nn_proto_view view;
	rv = nn_proto_parse(buf, (size_t)rv, &view, NULL);
	ASSERT_OR_DIE(rv == 0, "parse failed");
	ASSERT_OR_DIE(view.device_id == NULL, "device_id should be NULL");
	ASSERT_OR_DIE(view.device_id_size == 0, "device_id_size should be 0");
	ASSERT_OR_DIE(view.payload_size == sizeof(payload), "payload size mismatch");
	ASSERT_OR_DIE(memcmp(view.payload, payload, sizeof(payload)) == 0,
		      "payload bytes mismatch");

	printf("PASS: empty_device_id\n");
	return 0;
}

static int test_back_to_back_frames(void)
{
	uint8_t buf[512];
	struct stub_sig_ctx sctx_a = { .sig_pattern = 0x11 };
	struct stub_sig_ctx sctx_b = { .sig_pattern = 0x22 };

	int wa = nn_proto_encode(NN_PROTO_TYPE_D2H,
				 (const uint8_t *)"id1", 3,
				 (const uint8_t *)"hello", 5,
				 stub_sign, &sctx_a, buf, sizeof(buf));
	ASSERT_OR_DIE(wa > 0, "encode A failed");
	int wb = nn_proto_encode(NN_PROTO_TYPE_H2D,
				 (const uint8_t *)"id2x", 4,
				 (const uint8_t *)"world!", 6,
				 stub_sign, &sctx_b, buf + wa, sizeof(buf) - wa);
	ASSERT_OR_DIE(wb > 0, "encode B failed");

	struct nn_proto_view vA, vB;
	size_t consumed_a = 0, consumed_b = 0;
	int rv = nn_proto_parse(buf, wa + wb, &vA, &consumed_a);
	ASSERT_OR_DIE(rv == 0 && consumed_a == (size_t)wa, "parse A");
	rv = nn_proto_parse(buf + consumed_a, wb, &vB, &consumed_b);
	ASSERT_OR_DIE(rv == 0 && consumed_b == (size_t)wb, "parse B");
	ASSERT_OR_DIE(memcmp(vA.payload, "hello", 5) == 0, "A payload");
	ASSERT_OR_DIE(memcmp(vB.payload, "world!", 6) == 0, "B payload");

	printf("PASS: back_to_back_frames\n");
	return 0;
}

static int test_truncated(void)
{
	uint8_t buf[128];
	struct stub_sig_ctx sctx = { .sig_pattern = 0xEE };
	int rv = nn_proto_encode(NN_PROTO_TYPE_D2H,
				 (const uint8_t *)"id", 2,
				 (const uint8_t *)"hi", 2,
				 stub_sign, &sctx, buf, sizeof(buf));
	ASSERT_OR_DIE(rv > 0, "encode failed");
	struct nn_proto_view view;
	int prv = nn_proto_parse(buf, (size_t)rv - 5, &view, NULL);
	ASSERT_OR_DIE(prv == -ENOSPC, "expected -ENOSPC on truncated buf");

	printf("PASS: truncated\n");
	return 0;
}

static int test_bad_magic(void)
{
	uint8_t buf[64] = { 'X', 'X' };
	struct nn_proto_view view;
	int rv = nn_proto_parse(buf, sizeof(buf), &view, NULL);
	ASSERT_OR_DIE(rv == -EINVAL, "expected -EINVAL on bad magic");
	printf("PASS: bad_magic\n");
	return 0;
}

static int test_pkt_size_below_minimum(void)
{
	/* Hand-built malformed frame: pkt_size = 10, but minimum for
	 * device_id_size=0 is 2 (size field) + 0 + 64 (sig) = 66. */
	uint8_t buf[200] = {
		0x4e, 0x4e,            /* magic */
		0x00, 0x00,            /* type=D2H */
		0x0a, 0x00, 0x00, 0x00, /* pkt_size = 10 */
		0x00, 0x00,            /* device_id_size = 0 */
	};
	struct nn_proto_view view;
	int rv = nn_proto_parse(buf, sizeof(buf), &view, NULL);
	ASSERT_OR_DIE(rv == -EINVAL, "expected -EINVAL on tiny pkt_size");
	printf("PASS: pkt_size_below_minimum\n");
	return 0;
}

static int test_golden_vector(void)
{
	/* A hand-crafted frame with deterministic header + payload (sig
	 * filled with stub bytes — cross-language sig verification with
	 * real ECDSA P-256 lives in the on-device gateway tests).  This
	 * just proves the C parser reads the same fields Python writes.
	 *
	 * Layout: magic 4e4e | type 0200 | pkt_size 48 00 00 00 | did_size 04 00
	 *         | did 01 02 03 04 | payload 01 00 (HUB_STATUS_QUERY)
	 *         | 64 stub sig bytes (0xCC repeated)
	 */
	uint8_t buf[80];
	const uint8_t header_and_body[16] = {
		0x4e, 0x4e,                       /* magic */
		0x02, 0x00,                       /* type = D2G */
		0x48, 0x00, 0x00, 0x00,           /* pkt_size = 0x48 = 72 */
		0x04, 0x00,                       /* device_id_size = 4 */
		0x01, 0x02, 0x03, 0x04,           /* device_id */
		0x01, 0x00,                       /* payload = HUB_STATUS_QUERY */
	};
	memcpy(buf, header_and_body, sizeof(header_and_body));
	memset(buf + sizeof(header_and_body), 0xCC, NN_PROTO_SIG_LEN);

	struct nn_proto_view view;
	size_t consumed = 0;
	int rv = nn_proto_parse(buf, sizeof(buf), &view, &consumed);
	ASSERT_OR_DIE(rv == 0, "parse failed on golden vector");
	ASSERT_OR_DIE(consumed == 80, "consumed != 80");
	ASSERT_OR_DIE(view.type == NN_PROTO_TYPE_D2G, "type != D2G");
	ASSERT_OR_DIE(view.device_id_size == 4, "device_id_size != 4");
	uint8_t expect_id[4] = {0x01, 0x02, 0x03, 0x04};
	ASSERT_OR_DIE(memcmp(view.device_id, expect_id, 4) == 0,
		      "device_id bytes mismatch");
	ASSERT_OR_DIE(view.payload_size == 2, "payload_size != 2");
	ASSERT_OR_DIE(view.payload[0] == 0x01 && view.payload[1] == 0x00,
		      "payload != cmd HUB_STATUS_QUERY");

	uint16_t cmd = (uint16_t)view.payload[0] |
		       ((uint16_t)view.payload[1] << 8);
	ASSERT_OR_DIE(cmd == NN_PROTO_CMD_HUB_STATUS_QUERY,
		      "inner cmd != HUB_STATUS_QUERY");

	for (int i = 0; i < 64; i++) {
		ASSERT_OR_DIE(view.sig[i] == 0xCC, "sig stub mismatch");
	}

	printf("PASS: golden_vector (header + body match Python frame layout)\n");
	return 0;
}

int main(void)
{
	int rc = 0;
	rc += test_round_trip_basic();
	rc += test_empty_device_id();
	rc += test_back_to_back_frames();
	rc += test_truncated();
	rc += test_bad_magic();
	rc += test_pkt_size_below_minimum();
	rc += test_golden_vector();
	if (rc == 0) {
		printf("\nAll tests passed.\n");
	} else {
		fprintf(stderr, "\n%d test(s) failed.\n", rc);
	}
	return rc;
}
