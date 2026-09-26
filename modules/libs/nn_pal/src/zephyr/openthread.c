/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_pal/openthread.h>

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <nn_osal/osal.h>

#include <zephyr/net/openthread.h>
#include <openthread/instance.h>
#include <openthread/ip6.h>
#include <openthread/thread.h>
#include <openthread/dataset.h>

NN_OSAL_LOG_MODULE(nn_pal_ot);

#define MAX_STATE_CBS 4

struct cb_slot {
    nn_pal_ot_state_cb_t fn;
    void                *user;
    bool                 in_use;
};

static struct cb_slot s_cbs[MAX_STATE_CBS];
static bool           s_state_cb_registered;
static bool           s_inited;

static void state_change_trampoline(otChangedFlags flags, void *ctx)
{
    (void)ctx;
    for (int i = 0; i < MAX_STATE_CBS; i++) {
        if (s_cbs[i].in_use && s_cbs[i].fn) {
            s_cbs[i].fn((uint32_t)flags, s_cbs[i].user);
        }
    }
}

int nn_pal_ot_init(void)
{
    /* OT comes up automatically on Zephyr; this just verifies the
     * default instance exists. */
    if (s_inited) return 0;
    otInstance *inst = openthread_get_default_instance();
    if (!inst) return -ENODEV;
    s_inited = true;
    return 0;
}

nn_pal_ot_instance_t nn_pal_ot_instance(void)
{
    return (nn_pal_ot_instance_t)openthread_get_default_instance();
}

void nn_pal_ot_mutex_lock(void)
{
    openthread_api_mutex_lock(openthread_get_default_context());
}

void nn_pal_ot_mutex_unlock(void)
{
    openthread_api_mutex_unlock(openthread_get_default_context());
}

int nn_pal_ot_iface_start(void)
{
    otInstance *inst = openthread_get_default_instance();
    if (!inst) return -ENODEV;

    nn_pal_ot_mutex_lock();
    otError e1 = otIp6SetEnabled(inst, true);
    otError e2 = OT_ERROR_NONE;
    if (e1 == OT_ERROR_NONE) {
        e2 = otThreadSetEnabled(inst, true);
    }
    if (!s_state_cb_registered) {
        otError er = otSetStateChangedCallback(inst,
                                               state_change_trampoline,
                                               NULL);
        if (er == OT_ERROR_NONE || er == OT_ERROR_ALREADY) {
            s_state_cb_registered = true;
        }
    }
    nn_pal_ot_mutex_unlock();

    if (e1 != OT_ERROR_NONE || e2 != OT_ERROR_NONE) {
        NN_LOG_WRN("ot_iface_start: ip6=%d thread=%d", e1, e2);
        return -EIO;
    }
    return 0;
}

int nn_pal_ot_iface_stop(void)
{
    otInstance *inst = openthread_get_default_instance();
    if (!inst) return -ENODEV;
    nn_pal_ot_mutex_lock();
    otThreadSetEnabled(inst, false);
    otIp6SetEnabled(inst, false);
    nn_pal_ot_mutex_unlock();
    return 0;
}

bool nn_pal_ot_is_commissioned(void)
{
    otInstance *inst = openthread_get_default_instance();
    if (!inst) return false;
    nn_pal_ot_mutex_lock();
    bool yes = otDatasetIsCommissioned(inst);
    nn_pal_ot_mutex_unlock();
    return yes;
}

int nn_pal_ot_state_cb_register(nn_pal_ot_state_cb_t cb, void *user)
{
    if (!cb) return -EINVAL;
    for (int i = 0; i < MAX_STATE_CBS; i++) {
        if (!s_cbs[i].in_use) {
            s_cbs[i].fn     = cb;
            s_cbs[i].user   = user;
            s_cbs[i].in_use = true;
            return 0;
        }
    }
    return -ENOMEM;
}

int nn_pal_ot_state_cb_unregister(nn_pal_ot_state_cb_t cb, void *user)
{
    for (int i = 0; i < MAX_STATE_CBS; i++) {
        if (s_cbs[i].in_use && s_cbs[i].fn == cb && s_cbs[i].user == user) {
            s_cbs[i].in_use = false;
            s_cbs[i].fn     = NULL;
            s_cbs[i].user   = NULL;
            return 0;
        }
    }
    return -ENOENT;
}

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_l2.h>

uint32_t nn_pal_ot_iface_scope_id(void)
{
    struct net_if *iface =
        net_if_get_first_by_type(&NET_L2_GET_NAME(OPENTHREAD));
    if (!iface) return 0;
    return net_if_get_by_iface(iface);
}
