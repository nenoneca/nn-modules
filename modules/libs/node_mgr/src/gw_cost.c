/*
 * gw_cost -- mesh path cost from this device to a gateway (by RLOC16), for
 * the nearest-gateway rule in gw_policy.c.  Overrides the weak default in
 * nn_proto_client.c (which returns NN_GW_COST_UNKNOWN: no nearer-switches).
 */
#include <node_mgr/gw_policy.h>
#include <nn_pal/openthread.h>
#include <zephyr/net/openthread.h>   /* openthread_get_default_context, api mutex */
#include <openthread.h>          /* openthread_mutex_lock/unlock (non-deprecated API) */

#include <openthread/instance.h>
#include <openthread/thread_ftd.h>

uint8_t nn_proto_client_gw_cost(uint16_t rloc16)
{
	if (rloc16 == 0xFFFE) {
		return NN_GW_COST_UNKNOWN;
	}
	struct openthread_context *octx = openthread_get_default_context();
	otInstance *ot = openthread_get_default_instance();
	if (!octx || !ot) {
		return NN_GW_COST_UNKNOWN;
	}
	uint8_t cost = NN_GW_COST_NONE;
	openthread_mutex_lock();
	otThreadGetNextHopAndPathCost(ot, rloc16, NULL, &cost);
	openthread_mutex_unlock();
	return cost > NN_GW_COST_NONE ? NN_GW_COST_NONE : cost;
}
