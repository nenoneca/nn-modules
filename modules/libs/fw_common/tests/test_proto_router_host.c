/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Host-side standalone test for fw_common/proto_router snapshot API.
 * Compile + run on Linux:
 *
 *     gcc -std=c11 -Wall -Wextra -O0 -g \
 *         -pthread \
 *         -I include \
 *         -I ../nn_proto/include \
 *         tests/test_proto_router_host.c \
 *         src/proto_router.c \
 *         ../nn_proto/src/nn_proto.c \
 *         -o /tmp/proto_router_test && /tmp/proto_router_test
 *
 * Covers proto_router_snapshot() — the gateway's mechanism for handing
 * its cached device_id → ml_eid map to the heartbeat emitter so the hub
 * can self-heal each device's ml_eid after sensor reboot.  See
 * feedback_mleid_drift_breaks_d2d.md.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <fw_common/proto_router.h>

/* Stubs for the TCP/UDP I/O paths in proto_router.c.  This test exercises
 * the routing-table API (remember/lookup/snapshot) only; the dispatch
 * paths are out of scope and use a real network. */
int proto_tcp_enqueue(const uint8_t *f, size_t n) { (void)f; (void)n; return 0; }
int proto_udp_send_unicast(const struct in6_addr *d, uint16_t p,
                           const uint8_t *f, size_t n) {
	(void)d; (void)p; (void)f; (void)n; return 0;
}
int proto_udp_send_mcast(const uint8_t *f, size_t n) {
	(void)f; (void)n; return 0;
}

static struct in6_addr addr_of(const char *s)
{
	struct in6_addr a;
	int rc = inet_pton(AF_INET6, s, &a);
	assert(rc == 1);
	return a;
}

static void test_snapshot_empty(void)
{
	proto_router_init();
	struct proto_router_entry buf[8];
	int n = proto_router_snapshot(buf, 8);
	assert(n == 0);
	printf("  empty snapshot returns 0 ............ OK\n");
}

static void test_snapshot_returns_remembered_entries(void)
{
	proto_router_init();

	uint8_t did_a[8] = {0xaa,0xbb,0xcc,0xdd,0x00,0x01,0x02,0x03};
	uint8_t did_b[8] = {0x11,0x22,0x33,0x44,0xff,0xee,0xdd,0xcc};
	struct in6_addr addr_a = addr_of("fd44:8b73:6d00:1:e008:e670:2cc7:29bf");
	struct in6_addr addr_b = addr_of("fd44:8b73:6d00:1:1b0d:cf77:114:d66e");

	assert(proto_router_remember(did_a, 8, &addr_a) == 0);
	assert(proto_router_remember(did_b, 8, &addr_b) == 0);

	struct proto_router_entry buf[PROTO_ROUTER_TABLE_SIZE];
	int n = proto_router_snapshot(buf, PROTO_ROUTER_TABLE_SIZE);
	assert(n == 2);

	/* Verify both DIDs appear, addresses match (order isn't guaranteed). */
	int found_a = 0, found_b = 0;
	for (int i = 0; i < n; i++) {
		assert(buf[i].did_size == 8);
		if (memcmp(buf[i].device_id, did_a, 8) == 0) {
			assert(memcmp(&buf[i].addr, &addr_a, sizeof addr_a) == 0);
			found_a = 1;
		} else if (memcmp(buf[i].device_id, did_b, 8) == 0) {
			assert(memcmp(&buf[i].addr, &addr_b, sizeof addr_b) == 0);
			found_b = 1;
		}
	}
	assert(found_a && found_b);
	printf("  snapshot returns both remembered .... OK\n");
}

static void test_snapshot_respects_max(void)
{
	proto_router_init();
	for (int i = 0; i < 5; i++) {
		uint8_t did[8] = {0};
		did[0] = (uint8_t)i;
		struct in6_addr a = addr_of("fd44::1");
		assert(proto_router_remember(did, 8, &a) == 0);
	}
	struct proto_router_entry buf[3];
	int n = proto_router_snapshot(buf, 3);
	assert(n == 3);
	printf("  snapshot caps at max=3 .............. OK\n");
}

static void test_snapshot_rejects_invalid(void)
{
	proto_router_init();
	struct proto_router_entry buf[1];
	assert(proto_router_snapshot(NULL, 1) == -EINVAL);
	assert(proto_router_snapshot(buf, 0) == -EINVAL);
	assert(proto_router_snapshot(buf, -1) == -EINVAL);
	printf("  invalid args → -EINVAL .............. OK\n");
}

static void test_snapshot_after_re_remember(void)
{
	/* re-remember with a new address; snapshot should show fresh address. */
	proto_router_init();
	uint8_t did[8] = {0xde,0xad,0xbe,0xef,0,0,0,0};
	struct in6_addr stale = addr_of("fd00:5a1e::1");
	struct in6_addr fresh = addr_of("fd44:f3e5::1");
	assert(proto_router_remember(did, 8, &stale) == 0);
	assert(proto_router_remember(did, 8, &fresh) == 0);

	struct proto_router_entry buf[PROTO_ROUTER_TABLE_SIZE];
	int n = proto_router_snapshot(buf, PROTO_ROUTER_TABLE_SIZE);
	assert(n == 1);
	assert(memcmp(&buf[0].addr, &fresh, sizeof fresh) == 0);
	printf("  re-remember updates address ......... OK\n");
}

int main(void)
{
	printf("test_proto_router_host:\n");
	test_snapshot_empty();
	test_snapshot_returns_remembered_entries();
	test_snapshot_respects_max();
	test_snapshot_rejects_invalid();
	test_snapshot_after_re_remember();
	printf("all OK\n");
	return 0;
}
