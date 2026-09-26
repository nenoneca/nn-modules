/* SPDX-License-Identifier: Apache-2.0 */
/* See gw_policy.h.  This file must stay free of OS/OSAL includes so the
 * host test compiles it with bare gcc. */
#include <node_mgr/gw_policy.h>

#include <string.h>

uint16_t nn_gw_hello_rloc16(const uint8_t *args, size_t args_len)
{
	if (args_len < 21) {
		return 0xFFFE;
	}
	return (uint16_t)(args[19] | ((uint16_t)args[20] << 8));
}

static bool cost_valid(uint8_t c)
{
	return c < NN_GW_COST_NONE;
}

static void adopt(nn_gw_state_t *st, const uint8_t addr[16], uint16_t interval_s,
		  bool online, uint64_t now_ms, uint8_t cost)
{
	memcpy(st->addr, addr, 16);
	st->last_seen_ms = now_ms;
	st->interval_s   = interval_s;
	st->hub_online   = online;
	st->known        = true;
	st->cost         = cost;
	st->cand_count   = 0;
}

nn_gw_verdict_t nn_gw_policy_hello_cost(nn_gw_state_t *st,
					const uint8_t *args, size_t args_len,
					const uint8_t src_addr[16],
					uint64_t now_ms, uint8_t cost)
{
	if (args_len < 19) {
		return NN_GW_MALFORMED;
	}
	uint8_t addr[16];
	memcpy(addr, args, 16);
	bool zero_addr = true;
	for (int i = 0; i < 16; i++) {
		if (addr[i]) { zero_addr = false; break; }
	}
	if (zero_addr) {
		memcpy(addr, src_addr, 16);
	}
	uint16_t interval_s = (uint16_t)(args[16] | ((uint16_t)args[17] << 8));
	bool     online     = args[18] != 0;
	if (interval_s < 5 || interval_s > 600) {
		interval_s = 30;               /* sane default / bad field */
	}

	if (!st->known) {
		adopt(st, addr, interval_s, online, now_ms, cost);
		st->last_switch_ms = 0;        /* first optimisation may follow at once */
		return NN_GW_ADOPT_FIRST;
	}
	if (memcmp(addr, st->addr, 16) == 0) {
		st->last_seen_ms = now_ms;
		st->interval_s   = interval_s;
		st->hub_online   = online;
		st->cost         = cost;
		return NN_GW_REFRESH;
	}

	/* A rival.  STICKY while the current gateway's HELLOs are fresh
	 * (3x ITS advertised interval) -- unless the rival reports hub-online
	 * and the current gateway does not: a gateway that lost its hub
	 * uplink must not hold the fleet. */
	uint64_t fresh_ms = (uint64_t)st->interval_s * 3u * 1000u;
	bool cur_fresh = (now_ms - st->last_seen_ms) < fresh_ms;
	bool rival_wins_on_hub = online && !st->hub_online;
	if (!cur_fresh || rival_wins_on_hub) {
		adopt(st, addr, interval_s, online, now_ms, cost);
		st->last_switch_ms = now_ms;
		return rival_wins_on_hub ? NN_GW_SWITCH_HUB : NN_GW_SWITCH_STALE;
	}

	/* Nearest gateway: the rival must be hub-online, reachable and cheaper
	 * by the margin on N HELLOs in a row; ties never switch. */
	bool cur_no_route = st->cost == NN_GW_COST_NONE;
	bool nearer = online && cost_valid(cost) &&
		      (cur_no_route ||
		       (cost_valid(st->cost) &&
			(unsigned)cost + NN_GW_NEARER_MARGIN <= st->cost));
	if (!nearer) {
		if (st->cand_count && memcmp(st->cand_addr, addr, 16) == 0) {
			st->cand_count = 0;    /* no longer nearer: start over */
		}
		return NN_GW_KEEP_CURRENT;
	}
	if (st->cand_count && memcmp(st->cand_addr, addr, 16) == 0) {
		if (st->cand_count < 255) st->cand_count++;
	} else {
		memcpy(st->cand_addr, addr, 16);
		st->cand_count = 1;
	}
	bool dwell_ok = cur_no_route || st->last_switch_ms == 0 ||
			(now_ms - st->last_switch_ms) >= NN_GW_MIN_DWELL_MS;
	if (st->cand_count >= NN_GW_NEARER_HELLOS && dwell_ok) {
		adopt(st, addr, interval_s, online, now_ms, cost);
		st->last_switch_ms = now_ms;
		return NN_GW_SWITCH_NEARER;
	}
	return NN_GW_KEEP_CURRENT;
}

nn_gw_verdict_t nn_gw_policy_hello(nn_gw_state_t *st,
				   const uint8_t *args, size_t args_len,
				   const uint8_t src_addr[16],
				   uint64_t now_ms)
{
	return nn_gw_policy_hello_cost(st, args, args_len, src_addr, now_ms,
				       NN_GW_COST_UNKNOWN);
}
