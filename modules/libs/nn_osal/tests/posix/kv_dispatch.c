/* SPDX-License-Identifier: Apache-2.0 */
/* kv dispatch contract — the exact class of the v0.0.5 defect, where a
 * suffix/prefix mismatch meant load_all() dispatched NOTHING and no
 * setting ever persisted across reboot.
 *
 * Contract: keys are "<prefix>/<subkey>"; load_all() walks each
 * REGISTERED prefix and hands (subkey, data) — the subkey alone, not
 * the full key — to that prefix's callback and no other. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_osal/storage.h"

static char seen_a[8][32]; static int n_a;
static char seen_b[8][32]; static int n_b;
static char val_a[8][32];

static int cb_a(const char *sub, const uint8_t *d, size_t n, void *u)
{
    (void)u;
    snprintf(seen_a[n_a], 32, "%s", sub);
    snprintf(val_a[n_a], 32, "%.*s", (int)n, (const char *)d);
    n_a++;
    return 0;
}
static int cb_b(const char *sub, const uint8_t *d, size_t n, void *u)
{ (void)d; (void)n; (void)u; snprintf(seen_b[n_b++], 32, "%s", sub); return 0; }

static int has(char arr[8][32], int n, const char *want)
{
    for (int i = 0; i < n; i++) if (strcmp(arr[i], want) == 0) return 1;
    return 0;
}

int main(void)
{
    char tmpl[] = "/tmp/nn_kv_test_XXXXXX";
    assert(mkdtemp(tmpl));
    setenv("NN_OSAL_KV_DIR", tmpl, 1);

    assert(nn_osal_kv_init() == 0);
    assert(nn_osal_kv_register("alpha", cb_a, NULL) == 0);
    assert(nn_osal_kv_register("beta", cb_b, NULL) == 0);

    assert(nn_osal_kv_save("alpha/k1", "v1", 2) == 0);
    assert(nn_osal_kv_save("alpha/k2", "v2", 2) == 0);
    assert(nn_osal_kv_save("beta/k1", "bx", 2) == 0);
    assert(nn_osal_kv_save("gamma/k1", "??", 2) == 0);  /* unregistered */

    /* malformed keys are refused, not silently mangled */
    assert(nn_osal_kv_save("noprefix", "x", 1) == -EINVAL);
    assert(nn_osal_kv_save("a/b/c", "x", 1) == -EINVAL);
    assert(nn_osal_kv_save("/nosub", "x", 1) == -EINVAL);

    assert(nn_osal_kv_load_all() == 0);

    /* each callback saw ITS OWN subkeys — bare subkey, right data */
    assert(n_a == 2 && has(seen_a, n_a, "k1") && has(seen_a, n_a, "k2"));
    for (int i = 0; i < n_a; i++) {
        assert(strchr(seen_a[i], '/') == NULL);        /* subkey only */
        assert(strcmp(val_a[i], strcmp(seen_a[i], "k1") == 0 ? "v1" : "v2") == 0);
    }
    /* beta got beta's, and NOT alpha's or the unregistered gamma's */
    assert(n_b == 1 && strcmp(seen_b[0], "k1") == 0);

    /* delete → gone on the next load_all */
    assert(nn_osal_kv_delete("alpha/k1") == 0);
    n_a = 0; n_b = 0;
    assert(nn_osal_kv_load_all() == 0);
    assert(n_a == 1 && strcmp(seen_a[0], "k2") == 0);

    /* deleting a missing key is idempotent, not an error */
    assert(nn_osal_kv_delete("alpha/k1") == 0);

    printf("nn_osal kv dispatch: all contract tests passed\n");
    return 0;
}
