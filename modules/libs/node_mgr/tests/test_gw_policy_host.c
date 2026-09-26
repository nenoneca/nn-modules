/* SPDX-License-Identifier: Apache-2.0 */
/* Host test for the multi-gateway HELLO policy.  Pins the rules that
 * were measured on hardware 2026-08-28 (route flip-flop 60/40 → ZERO,
 * failover 89 s ≤ the 3-interval bound, rejoin → NO flip-back).
 *
 *     gcc -std=c11 -Wall -Wextra -O0 -g -I include \
 *         tests/test_gw_policy_host.c src/gw_policy.c \
 *         -o /tmp/gw_policy_test && /tmp/gw_policy_test
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <node_mgr/gw_policy.h>

static const uint8_t GW_A[16] = {0xfd, 1, [15] = 0xAA};
static const uint8_t GW_B[16] = {0xfd, 1, [15] = 0xBB};
static const uint8_t SRC[16]  = {0xfe, 0x80, [15] = 0x99};

static size_t hello(uint8_t *buf, const uint8_t addr[16],
                    uint16_t interval_s, int online)
{
    memcpy(buf, addr, 16);
    buf[16] = (uint8_t)(interval_s & 0xff);
    buf[17] = (uint8_t)(interval_s >> 8);
    buf[18] = (uint8_t)online;
    return 19;
}

int main(void)
{
    nn_gw_state_t st = {0};
    uint8_t h[19] = {0};

    /* malformed: too short, state untouched */
    assert(nn_gw_policy_hello(&st, h, 18, SRC, 1000) == NN_GW_MALFORMED);
    assert(!st.known);

    /* first HELLO adopts, parses interval + online */
    hello(h, GW_A, 30, 1);
    assert(nn_gw_policy_hello(&st, h, 19, SRC, 1000) == NN_GW_ADOPT_FIRST);
    assert(st.known && st.interval_s == 30 && st.hub_online);
    assert(memcmp(st.addr, GW_A, 16) == 0);

    /* same gateway refreshes */
    assert(nn_gw_policy_hello(&st, h, 19, SRC, 31000) == NN_GW_REFRESH);
    assert(st.last_seen_ms == 31000);

    /* STICKY: rival B while A is fresh (within 3x30s) → ignored,
     * state untouched.  This is the flip-flop killer. */
    hello(h, GW_B, 30, 1);
    assert(nn_gw_policy_hello(&st, h, 19, SRC, 31000 + 89000) ==
           NN_GW_KEEP_CURRENT);
    assert(memcmp(st.addr, GW_A, 16) == 0);
    assert(st.last_seen_ms == 31000);           /* untouched */

    /* FAILOVER: A stale (>= 3x interval) → next B HELLO takes over.
     * 90 s bound == what the drill measured (89 s). */
    assert(nn_gw_policy_hello(&st, h, 19, SRC, 31000 + 90000) ==
           NN_GW_SWITCH_STALE);
    assert(memcmp(st.addr, GW_B, 16) == 0);

    /* NO FLIP-BACK: A rejoins while B is fresh → ignored */
    hello(h, GW_A, 30, 1);
    assert(nn_gw_policy_hello(&st, h, 19, SRC, 121001 + 1000) ==
           NN_GW_KEEP_CURRENT);
    assert(memcmp(st.addr, GW_B, 16) == 0);

    /* HUB-ONLINE PREEMPTION: current loses its hub uplink; a rival that
     * HAS the hub takes over even while current is fresh.  (This flag
     * was once parsed and dropped.) */
    hello(h, GW_B, 30, 0);                       /* B: hub offline */
    assert(nn_gw_policy_hello(&st, h, 19, SRC, 130000) == NN_GW_REFRESH);
    assert(!st.hub_online);
    hello(h, GW_A, 30, 1);                       /* A: hub online */
    assert(nn_gw_policy_hello(&st, h, 19, SRC, 131000) == NN_GW_SWITCH_HUB);
    assert(memcmp(st.addr, GW_A, 16) == 0 && st.hub_online);

    /* but NOT the reverse: a hub-offline rival never preempts a
     * hub-online fresh gateway */
    hello(h, GW_B, 30, 0);
    assert(nn_gw_policy_hello(&st, h, 19, SRC, 132000) == NN_GW_KEEP_CURRENT);

    /* zero-addr placeholder falls back to the mcast source address */
    nn_gw_state_t st2 = {0};
    uint8_t zero[16] = {0};
    hello(h, zero, 30, 1);
    assert(nn_gw_policy_hello(&st2, h, 19, SRC, 1000) == NN_GW_ADOPT_FIRST);
    assert(memcmp(st2.addr, SRC, 16) == 0);

    /* interval clamps: <5 or >600 → 30; stickiness follows the
     * CURRENT gateway's advertised interval (5 s → 15 s window) */
    nn_gw_state_t st3 = {0};
    hello(h, GW_A, 3, 1);
    nn_gw_policy_hello(&st3, h, 19, SRC, 0);
    assert(st3.interval_s == 30);
    hello(h, GW_A, 601, 1);
    nn_gw_policy_hello(&st3, h, 19, SRC, 0);
    assert(st3.interval_s == 30);
    hello(h, GW_A, 5, 1);
    nn_gw_policy_hello(&st3, h, 19, SRC, 0);
    assert(st3.interval_s == 5);
    hello(h, GW_B, 30, 1);
    assert(nn_gw_policy_hello(&st3, h, 19, SRC, 14999) == NN_GW_KEEP_CURRENT);
    assert(nn_gw_policy_hello(&st3, h, 19, SRC, 15000) == NN_GW_SWITCH_STALE);

    /* stale with NO rival: the same gateway's late HELLO refreshes —
     * staleness alone never drops a known gateway */
    nn_gw_state_t st4 = {0};
    hello(h, GW_A, 30, 1);
    nn_gw_policy_hello(&st4, h, 19, SRC, 0);
    assert(nn_gw_policy_hello(&st4, h, 19, SRC, 500000) == NN_GW_REFRESH);
    assert(st4.known);

    printf("gw_policy: all contract tests passed\n");
    
    /* ── nearest gateway ─────────────────────────────────────────── */
    {
        nn_gw_state_t n = {0};
        uint8_t b[19];
        uint64_t t = 1000;
        hello(b, GW_A, 5, 1);
        assert(nn_gw_policy_hello_cost(&n, b, 19, SRC, t, 3) == NN_GW_ADOPT_FIRST);
        assert(n.cost == 3);
        /* B one hop nearer: needs 3 HELLOs in a row, then switches at once
         * (no dwell after the first adoption) */
        hello(b, GW_B, 5, 1);
        assert(nn_gw_policy_hello_cost(&n, b, 19, SRC, t += 1000, 2) == NN_GW_KEEP_CURRENT);
        hello(b, GW_A, 5, 1);
        assert(nn_gw_policy_hello_cost(&n, b, 19, SRC, t += 1000, 3) == NN_GW_REFRESH);
        hello(b, GW_B, 5, 1);
        assert(nn_gw_policy_hello_cost(&n, b, 19, SRC, t += 4000, 2) == NN_GW_KEEP_CURRENT);
        assert(nn_gw_policy_hello_cost(&n, b, 19, SRC, t += 5000, 2) == NN_GW_SWITCH_NEARER);
        assert(memcmp(n.addr, GW_B, 16) == 0 && n.cost == 2);
        /* A now nearer again: 3 HELLOs, but the 5-min dwell holds */
        hello(b, GW_B, 5, 1);
        hello(b, GW_A, 5, 1);
        for (int k = 0; k < 5; k++)
            assert(nn_gw_policy_hello_cost(&n, b, 19, SRC, t += 1000, 1) == NN_GW_KEEP_CURRENT);
        hello(b, GW_B, 5, 1);
        assert(nn_gw_policy_hello_cost(&n, b, 19, SRC, t, 2) == NN_GW_REFRESH);
        hello(b, GW_A, 5, 1);
        t = n.last_switch_ms + NN_GW_MIN_DWELL_MS;
        hello(b, GW_B, 5, 1);
        assert(nn_gw_policy_hello_cost(&n, b, 19, SRC, t - 1000, 2) == NN_GW_REFRESH);
        hello(b, GW_A, 5, 1);
        assert(nn_gw_policy_hello_cost(&n, b, 19, SRC, t, 1) == NN_GW_SWITCH_NEARER);
        /* ties never switch */
        hello(b, GW_B, 5, 1);
        for (int k = 0; k < 10; k++)
            assert(nn_gw_policy_hello_cost(&n, b, 19, SRC, t += 1000, 1) == NN_GW_KEEP_CURRENT);
        /* a rival that stops being nearer resets its streak */
        nn_gw_state_t m = {0};
        hello(b, GW_A, 5, 1);
        nn_gw_policy_hello_cost(&m, b, 19, SRC, 1000, 3);
        hello(b, GW_B, 5, 1);
        nn_gw_policy_hello_cost(&m, b, 19, SRC, 2000, 1);
        nn_gw_policy_hello_cost(&m, b, 19, SRC, 3000, 1);
        assert(nn_gw_policy_hello_cost(&m, b, 19, SRC, 4000, 3) == NN_GW_KEEP_CURRENT);
        assert(m.cand_count == 0);
        /* hub-offline rival is never "nearer"; unknown cost never switches */
        hello(b, GW_B, 5, 0);
        for (int k = 0; k < 5; k++)
            assert(nn_gw_policy_hello_cost(&m, b, 19, SRC, 5000 + k, 1) == NN_GW_KEEP_CURRENT);
        hello(b, GW_B, 5, 1);
        for (int k = 0; k < 5; k++)
            assert(nn_gw_policy_hello_cost(&m, b, 19, SRC, 6000 + k, NN_GW_COST_UNKNOWN) == NN_GW_KEEP_CURRENT);
        /* current lost its route: nearer rival switches without the dwell */
        m.last_switch_ms = 6000; m.cost = NN_GW_COST_NONE;
        for (int k = 0; k < 2; k++)
            assert(nn_gw_policy_hello_cost(&m, b, 19, SRC, 7000 + k, 2) == NN_GW_KEEP_CURRENT);
        assert(nn_gw_policy_hello_cost(&m, b, 19, SRC, 7002, 2) == NN_GW_SWITCH_NEARER);
        /* rloc16 in the HELLO */
        uint8_t r[21]; hello(r, GW_A, 5, 1); r[19] = 0x00; r[20] = 0x90;
        assert(nn_gw_hello_rloc16(r, 21) == 0x9000 && nn_gw_hello_rloc16(r, 19) == 0xFFFE);
    }
    printf("gw_policy nearest-gateway: OK\n");
    return 0;
}
