/* SPDX-License-Identifier: Apache-2.0 */
/* Multi-gateway HELLO policy — the pure decision core of
 * handle_gateway_hello(), extracted so the sticky/failover rules that
 * were measured on hardware (2026-08-28: flip-flop → zero, failover
 * 89 s, failback 52 s) are pinned by a host-compiled test instead of
 * living only in bench drills.
 *
 * No OS dependencies: callers supply the clock, the function owns the
 * parse + decision + state update.  See tests/test_gw_policy_host.c. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	bool     known;
	uint8_t  addr[16];
	uint64_t last_seen_ms;
	uint16_t interval_s;      /* the gateway's ADVERTISED HELLO period */
	bool     hub_online;
	/* nearest-gateway selection (nn_gw_policy_hello_cost) */
	uint8_t  cost;            /* mesh path cost to the current gateway */
	uint64_t last_switch_ms;  /* 0 = no voluntary-switch wait pending */
	uint8_t  cand_addr[16];   /* rival seen as nearer ... */
	uint8_t  cand_count;      /* ... this many HELLOs in a row */
} nn_gw_state_t;

/* Path cost values: OpenThread's route cost (0 = self, 1 per good link);
 * NN_GW_COST_NONE = no route, NN_GW_COST_UNKNOWN = not computed. */
#define NN_GW_COST_NONE        16
#define NN_GW_COST_UNKNOWN     0xFF
#define NN_GW_NEARER_MARGIN    1         /* at least this much cheaper ... */
#define NN_GW_NEARER_HELLOS    3         /* ... for this many HELLOs in a row */
#define NN_GW_MIN_DWELL_MS     300000u   /* at most one nearer-switch per 5 min */

typedef enum {
	NN_GW_MALFORMED,        /* short frame: state untouched */
	NN_GW_ADOPT_FIRST,      /* no gateway known yet */
	NN_GW_REFRESH,          /* same gateway: freshness/flags updated */
	NN_GW_KEEP_CURRENT,     /* sticky: rival ignored, state untouched */
	NN_GW_SWITCH_STALE,     /* current went stale → rival adopted */
	NN_GW_SWITCH_HUB,       /* rival hub-online, current lost hub */
	NN_GW_SWITCH_NEARER,    /* rival nearer by >= margin for N HELLOs */
} nn_gw_verdict_t;

/* HELLO body: addr[16] + interval_s le16 + hub_online u8 [+ rloc16 le16].
 * An all-zero addr falls back to src_addr (chips advertise the
 * placeholder before they know their own ML address).  interval
 * outside [5, 600] clamps to 30.  Stickiness window = 3x the CURRENT
 * gateway's advertised interval. */
nn_gw_verdict_t nn_gw_policy_hello(nn_gw_state_t *st,
				   const uint8_t *args, size_t args_len,
				   const uint8_t src_addr[16],
				   uint64_t now_ms);

/* Same, plus nearest-gateway selection: `cost` is the caller's mesh path
 * cost to the gateway that sent THIS HELLO (NN_GW_COST_UNKNOWN when the
 * HELLO carries no RLOC16).  A fresh, hub-online rival that is cheaper by
 * NN_GW_NEARER_MARGIN on NN_GW_NEARER_HELLOS consecutive HELLOs replaces
 * the current gateway -- at most once per NN_GW_MIN_DWELL_MS (the wait is
 * skipped when the current gateway has no route).  Ties never switch.
 * nn_gw_policy_hello() == this with cost unknown (no nearer-switches). */
nn_gw_verdict_t nn_gw_policy_hello_cost(nn_gw_state_t *st,
					const uint8_t *args, size_t args_len,
					const uint8_t src_addr[16],
					uint64_t now_ms, uint8_t cost);

/* RLOC16 appended to the HELLO by newer gateways (body >= 21 B), else
 * 0xFFFE. */
uint16_t nn_gw_hello_rloc16(const uint8_t *args, size_t args_len);

#ifdef __cplusplus
}
#endif
