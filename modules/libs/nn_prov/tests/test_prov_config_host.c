/* SPDX-License-Identifier: Apache-2.0 */
/* Host contract test for the nn_prov CONFIG/WIFI handlers.
 *
 *     gcc -std=c11 -Wall -Wextra -O0 -g \
 *         -I tests/fake_esp -I include -I src \
 *         tests/test_prov_config_host.c tests/fake_esp/fake_esp.c \
 *         src/nn_prov.c -o /tmp/prov_test && /tmp/prov_test
 *
 * Pins the CONFIG v1 wire layout, the trailing-TLV rules (version stays
 * 1: pre-TLV firmware ignores trailing bytes, so appending is the
 * backward-compatible extension point), the 0x47 gw_blob dormant store,
 * and the WIFI flow's ordering guarantee (SUCCESS notified BEFORE any
 * radio work; reboot deferred to a task). */
#define _DEFAULT_SOURCE   /* mkdtemp, setenv, dirent */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "nn_prov_internal.h"
#include "fake_esp_ctl.h"

/* Storage is the REAL posix nn_osal_kv backend (nn_prov was ported off
 * NVS onto it), rooted at a temp dir via NN_OSAL_KV_DIR.  These two
 * helpers replace the old fake-NVS peek: they read what actually landed
 * on disk, so a wrong key name or length fails here instead of shipping. */
static char g_kv_root[64];

static void kv_setup(void)
{
    snprintf(g_kv_root, sizeof g_kv_root, "/tmp/nn_prov_test_XXXXXX");
    if (!mkdtemp(g_kv_root)) { perror("mkdtemp"); exit(1); }
    setenv("NN_OSAL_KV_DIR", g_kv_root, 1);
}

static void kv_reset(void)
{
    char dir[96];
    snprintf(dir, sizeof dir, "%s/nnprov", g_kv_root);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char f[96 + 256];
            snprintf(f, sizeof f, "%s/%s", dir, de->d_name);
            unlink(f);
        }
        closedir(d);
    }
    fake_nvs_reset();          /* the rest of the fake state still applies */
}

static const uint8_t *kv_get(const char *key, size_t *len)
{
    static uint8_t buf[1024];
    char f[160];
    snprintf(f, sizeof f, "%s/nnprov/%s", g_kv_root, key);
    FILE *fp = fopen(f, "rb");
    if (!fp) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    buf[n] = 0;                /* so string values can be strcmp'd */
    *len = n;
    return buf;
}

static size_t put(uint8_t *b, size_t i, const void *d, size_t n)
{ memcpy(b + i, d, n); return i + n; }

/* Build a valid CONFIG v1 body; returns length. */
static size_t mk_config(uint8_t *b, const char *name,
                        const char *hub, uint16_t hub_port,
                        const char *stream, uint16_t stream_port)
{
    size_t i = 0;
    uint8_t hubpub[32]; memset(hubpub, 0xA1, 32);
    uint8_t strpub[32]; memset(strpub, 0xB2, 32);
    b[i++] = 1;
    b[i++] = (uint8_t)strlen(name);
    i = put(b, i, name, strlen(name));
    i = put(b, i, hubpub, 32);
    i = put(b, i, strpub, 32);
    b[i++] = (uint8_t)strlen(hub);
    i = put(b, i, hub, strlen(hub));
    b[i++] = (uint8_t)(hub_port >> 8); b[i++] = (uint8_t)hub_port;
    b[i++] = (uint8_t)strlen(stream);
    i = put(b, i, stream, strlen(stream));
    b[i++] = (uint8_t)(stream_port >> 8); b[i++] = (uint8_t)stream_port;
    return i;
}

static size_t add_tlv(uint8_t *b, size_t i, uint8_t t,
                      const void *v, uint16_t n)
{
    b[i++] = t;
    b[i++] = (uint8_t)(n >> 8); b[i++] = (uint8_t)n;
    return put(b, i, v, n);
}


/* Phase 2, run as a FRESH PROCESS against the kv dir phase 1 left behind.
 * It has to be a new process: in-process the statics still hold what the
 * write path just set, so a broken load path would pass unnoticed — which
 * is exactly how the missing nn_osal_kv_load_all() call went undetected. */
static int reload_phase(const char *dir)
{
    setenv("NN_OSAL_KV_DIR", dir, 1);
    assert(nn_prov_init() == ESP_OK);
    assert(nn_prov_is_provisioned());          /* the "done" flag survived */

    char host[64]; uint16_t port = 0;
    assert(nn_prov_get_hub_endpoint(host, sizeof host, &port));
    assert(strcmp(host, "h") == 0 && port == 1);

    uint8_t pub[32];
    assert(nn_prov_get_hub_pubkey(pub) && pub[0] == 0xA1);
    assert(nn_prov_get_stream_pubkey(pub) && pub[0] == 0xB2);
    printf("nn_prov reload-from-kv: OK\n");
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t b[900];
    size_t n;

    if (argc == 3 && strcmp(argv[1], "--reload") == 0)
        return reload_phase(argv[2]);

    kv_setup();

    /* — plain CONFIG v1 (pre-TLV shape) applies and persists — */
    kv_reset();
    n = mk_config(b, "cam9", "hub.local", 8770, "10.0.0.2", 8766);
    assert(nn_prov_handle_config(b, n) == ESP_OK);
    assert(strcmp(fake_stream_host, "10.0.0.2") == 0);
    assert(fake_stream_port == 8766);
    assert(fake_hub_pub_set[0] == 0xA1);          /* crypto got the hub key */
    assert(fake_stream_key[0] == 0xB2);           /* uplink got the stream key */
    size_t len; const uint8_t *v = kv_get("name", &len);
    assert(v && strcmp((const char *)v, "cam9") == 0);
    assert(fake_statuses[fake_status_count - 1] == NN_PROV_APPLYING);
    assert(kv_get("gw_blob", &len) == NULL); /* no TLV, no blob */

    /* — 0x47 TLV stores the dormant gateway blob byte-exact — */
    kv_reset();
    n = mk_config(b, "cam9", "hub.local", 8770, "10.0.0.2", 8766);
    const char *gw = "{\"gw_id\":\"abcd\"}";
    n = add_tlv(b, n, 0x47, gw, (uint16_t)strlen(gw));
    assert(nn_prov_handle_config(b, n) == ESP_OK);
    v = kv_get("gw_blob", &len);
    assert(v && len == strlen(gw) && memcmp(v, gw, len) == 0);

    /* — unknown TLV type is skipped; a 0x47 after it still lands — */
    kv_reset();
    n = mk_config(b, "cam9", "h", 1, "s", 2);
    n = add_tlv(b, n, 0x58, "????", 4);
    n = add_tlv(b, n, 0x47, gw, (uint16_t)strlen(gw));
    assert(nn_prov_handle_config(b, n) == ESP_OK);
    assert(kv_get("gw_blob", &len) != NULL);

    /* — truncated TLV stops cleanly: config still applies, no blob — */
    kv_reset();
    n = mk_config(b, "cam9", "h", 1, "s", 2);
    b[n++] = 0x47; b[n++] = 0x01; b[n++] = 0x00;   /* claims 256B, has 3 */
    b[n++] = 0xEE; b[n++] = 0xEE; b[n++] = 0xEE;
    assert(nn_prov_handle_config(b, n) == ESP_OK);
    assert(kv_get("gw_blob", &len) == NULL);

    /* — trailing garbage shorter than a TLV header is ignored (this IS
     * the pre-TLV compatibility rule, exercised in reverse) — */
    kv_reset();
    n = mk_config(b, "cam9", "h", 1, "s", 2);
    b[n++] = 0xFF; b[n++] = 0xFF;
    assert(nn_prov_handle_config(b, n) == ESP_OK);

    /* — malformed: wrong version, truncated body — */
    kv_reset();
    n = mk_config(b, "cam9", "h", 1, "s", 2);
    b[0] = 2;
    assert(nn_prov_handle_config(b, n) == ESP_ERR_INVALID_ARG);
    b[0] = 1;
    assert(nn_prov_handle_config(b, 40) == ESP_ERR_INVALID_SIZE);
    assert(fake_statuses[fake_status_count - 1] == NN_PROV_ERROR);

    /* — WIFI before CONFIG is refused (no hub key yet) — */
    kv_reset(); fake_crypto_ready = 0;
    assert(nn_prov_handle_wifi((const uint8_t *)"{}", 2)
           == ESP_ERR_INVALID_STATE);

    /* — WIFI happy path: creds applied, done flag set, SUCCESS notified,
     * reboot DEFERRED to a task (never inline: the notify must flush) — */
    n = mk_config(b, "cam9", "h", 1, "s", 2);
    assert(nn_prov_handle_config(b, n) == ESP_OK);
    uint8_t pt[64]; size_t i = 0;
    pt[i++] = 4; i = put(pt, i, "ssid", 4);
    pt[i++] = 2; i = put(pt, i, "pw", 2);
    memcpy(fake_decrypt_plain, pt, i); fake_decrypt_len = i;
    fake_decrypt_rc = ESP_OK;
    int restarts_before = fake_restarts;
    assert(nn_prov_handle_wifi((const uint8_t *)"{...}", 5) == ESP_OK);
    assert(strcmp(fake_wifi_ssid, "ssid") == 0);
    assert(strcmp(fake_wifi_pass, "pw") == 0);
    assert(fake_statuses[fake_status_count - 1] == NN_PROV_SUCCESS);
    assert(fake_tasks == 1);
    assert(fake_restarts == restarts_before);      /* not inline */
    v = kv_get("done", &len);
    assert(v && v[0] == 1);

    printf("nn_prov config/wifi: all contract tests passed\n");

    /* Re-exec against the kv dir just written: proves the config actually
     * comes BACK on a cold start, not merely that it was written. */
    char cmd[320];
    snprintf(cmd, sizeof cmd, "%s --reload %s", argv[0], g_kv_root);
    if (system(cmd) != 0) {
        fprintf(stderr, "reload phase FAILED\n");
        return 1;
    }
    return 0;
}
