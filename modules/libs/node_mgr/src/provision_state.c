/* SPDX-License-Identifier: Apache-2.0 */

#include <node_mgr/provision_state.h>

#include <errno.h>
#include <string.h>

#include <nn_osal/osal.h>

#include <nn_pal/openthread.h>

NN_OSAL_LOG_MODULE(prov_state);

static prov_state_t g_state = PROV_STATE_SETUP;
static bool         g_loaded;

/* ── NVS-backed load via nn_osal_kv ─────────────────────────────── */

#define KV_PREFIX  "prov_state"
#define KV_KEY     "prov_state/state"

static int load_cb(const char *suffix, const uint8_t *value, size_t len,
                   void *user)
{
    (void)user;
    if (strcmp(suffix, "state") == 0 && len == sizeof(uint8_t)) {
        uint8_t v = value[0];
        g_state = (v == PROV_STATE_PROVISIONED) ? PROV_STATE_PROVISIONED
                                                : PROV_STATE_SETUP;
        NN_LOG_INF("loaded state: %s",
                   g_state == PROV_STATE_PROVISIONED ? "provisioned"
                                                     : "setup");
    }
    return 0;
}

int prov_state_init(void)
{
    if (g_loaded) return 0;
    int rv = nn_osal_kv_register(KV_PREFIX, load_cb, NULL);
    if (rv) {
        NN_LOG_WRN("kv_register: %d (treating as SETUP)", rv);
        g_state = PROV_STATE_SETUP;
        g_loaded = true;
        return rv;
    }
    /* nn_osal_kv_register now load_subtree's its prefix internally, so
     * by here g_state reflects whatever was persisted (or stayed SETUP
     * if the key never existed). */
    g_loaded = true;

    /* Upgrade path: a sensor that was provisioned by a firmware version
     * predating prov_state has an OT dataset in NVS but no
     * prov_state/state key.  Infer PROVISIONED from OT itself and
     * persist so subsequent boots take the fast path without this
     * heuristic. */
    if (g_state == PROV_STATE_SETUP) {
        if (nn_pal_ot_is_commissioned()) {
            NN_LOG_INF("prov_state unset but OT has a committed dataset "
                       "→ upgrading to PROVISIONED");
            (void)prov_state_set(PROV_STATE_PROVISIONED);
        }
    }
    return 0;
}

prov_state_t prov_state_get(void)
{
    return g_state;
}

int prov_state_set(prov_state_t s)
{
    uint8_t v = (uint8_t)s;
    int rv = nn_osal_kv_save(KV_KEY, &v, sizeof v);
    if (rv) {
        NN_LOG_WRN("kv_save: %d", rv);
        return rv;
    }
    g_state = s;
    NN_LOG_INF("state → %s",
               s == PROV_STATE_PROVISIONED ? "provisioned" : "setup");
    return 0;
}

/* ── boot-time auto-attach decision ─────────────────────────────── */

bool prov_state_should_auto_attach(void)
{
    if (g_state != PROV_STATE_PROVISIONED) {
        NN_LOG_INF("state=setup → BLE provisioning");
        return false;
    }

    /* Defense in depth: validate the dataset really is in OT.  The
     * persisted state could have lied (NVS corruption, partial wipe,
     * bug in earlier ble_prov/done handling, etc.).  If OT disagrees,
     * self-correct: clear the state back to SETUP and fall through to
     * BLE provisioning. */
    if (!nn_pal_ot_is_commissioned()) {
        NN_LOG_WRN("state=provisioned but otDatasetIsCommissioned()=false "
                   "— self-correcting back to setup");
        (void)prov_state_set(PROV_STATE_SETUP);
        return false;
    }

    NN_LOG_INF("state=provisioned + dataset valid → auto-attach Thread");
    return true;
}
