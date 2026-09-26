/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_prov core — parses the BLE provisioning writes, persists the result to
 * NVS, and applies it (configures + starts the WiFi/TCP uplink).
 *
 * CONFIG characteristic (plaintext, version 1):
 *   u8  ver = 1
 *   u8  name_len ; name[name_len]
 *   u8  hub_x25519_pub[32]
 *   u8  stream_x25519_pub[32]
 *   u8  hub_host_len ; hub_host[..] ; u16 hub_port  (big-endian)
 *   u8  stream_host_len ; stream_host[..] ; u16 stream_port (big-endian)
 *
 * WIFI characteristic (ECIES v2 JSON envelope; plaintext payload):
 *   u8  ssid_len ; ssid[..] ; u8 pass_len ; pass[..]
 */
#include "nn_prov/nn_prov.h"
#include "nn_prov_crypto.h"
#include "nn_prov_internal.h"
#include "nn_netstream/nn_netstream.h"

#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_prov);
#include <nn_osal/storage.h>
#include <nn_osal/system.h>
#include <nn_osal/thread.h>
#include <nn_osal/time.h>
#include <stdio.h>
#include <string.h>


/* kv prefix; keys below are "nnprov/<name>".  Was an NVS namespace. */
#define KV_PREFIX  "nnprov"
#define KV_KEY(k)  KV_PREFIX "/" k

/* Provisioned identity / peer material (the WiFi creds + stream endpoint live
 * in nn_netstream's own "nnstream" namespace). */
static char     s_name[32];
static uint8_t  s_hub_pub[32];   static bool s_have_hub_pub;
static uint8_t  s_stream_pub[32]; static bool s_have_stream_pub;
static char     s_hub_host[40];  static uint16_t s_hub_port; static bool s_have_hub_ep;
static bool     s_provisioned;

static nn_prov_status_t s_status = NN_PROV_IDLE;

static void prov_reboot_task(void *a, void *b, void *c);

static void set_status(nn_prov_status_t st)
{
    s_status = st;
    nn_prov_ble_notify_status(st);
}

/* ── persistence (nn_osal_kv) ───────────────────────────────────────────── */
/*
 * kv is a register-then-load store: nn_osal_kv_register() attaches a
 * callback for a prefix and nn_osal_kv_load_all() then replays every
 * stored key through it.  That suits nn_prov, which already keeps each
 * field in a static and only re-reads at boot.
 *
 * NOTE: register does NOT load on the esp_idf or posix backends (only
 * Zephyr's does), so load_all() below is required, not belt-and-braces.
 */
static int cfg_kv_cb(const char *key, const uint8_t *val, size_t len,
                     void *user)
{
    (void)user;
    if (!strcmp(key, "name")) {
        if (len && len < sizeof s_name) {
            memcpy(s_name, val, len);
            s_name[len] = '\0';
        }
    } else if (!strcmp(key, "hub_pub")) {
        if (len == 32) { memcpy(s_hub_pub, val, 32); s_have_hub_pub = true; }
    } else if (!strcmp(key, "stream_pub")) {
        if (len == 32) { memcpy(s_stream_pub, val, 32); s_have_stream_pub = true; }
    } else if (!strcmp(key, "hub_host")) {
        if (len && len < sizeof s_hub_host) {
            memcpy(s_hub_host, val, len);
            s_hub_host[len] = '\0';
        }
    } else if (!strcmp(key, "hub_port")) {
        if (len == sizeof s_hub_port) memcpy(&s_hub_port, val, len);
    } else if (!strcmp(key, "done")) {
        if (len == 1) s_provisioned = (val[0] == 1);
    }
    return 0;
}

static void cfg_load(void)
{
    (void)nn_osal_kv_init();
    int rv = nn_osal_kv_register(KV_PREFIX, cfg_kv_cb, NULL);
    if (rv) {
        NN_LOG_WRN("kv_register: %d (treating as unprovisioned)", rv);
        return;
    }
    nn_osal_kv_load_all();
    /* host and port are only usable together — a half-written pair means
     * no endpoint, same as the old two-step nvs_get_str/get_u16 test. */
    s_have_hub_ep = (s_hub_host[0] != '\0' && s_hub_port != 0);
}

static void cfg_save_config(void)
{
    nn_osal_kv_save(KV_KEY("name"), s_name, strlen(s_name));
    nn_osal_kv_save(KV_KEY("hub_pub"), s_hub_pub, 32);
    nn_osal_kv_save(KV_KEY("stream_pub"), s_stream_pub, 32);
    nn_osal_kv_save(KV_KEY("hub_host"), s_hub_host, strlen(s_hub_host));
    nn_osal_kv_save(KV_KEY("hub_port"), &s_hub_port, sizeof s_hub_port);
}

static void cfg_mark_done(void)
{
    uint8_t done = 1;
    nn_osal_kv_save(KV_KEY("done"), &done, 1);
    s_provisioned = true;
}

/* ── Provisioning write handlers ────────────────────────────────────────── */

esp_err_t nn_prov_handle_config(const uint8_t *d, size_t n)
{
    size_t i = 0;
    if (n < 2 || d[i++] != 1) { NN_LOG_ERR("CONFIG: bad header"); set_status(NN_PROV_ERROR); return ESP_ERR_INVALID_ARG; }

    uint8_t name_len = d[i++];
    if (i + name_len + 64 + 1 > n) goto malformed;
    size_t cl = name_len < sizeof s_name - 1 ? name_len : sizeof s_name - 1;
    memcpy(s_name, d + i, cl); s_name[cl] = '\0'; i += name_len;

    memcpy(s_hub_pub, d + i, 32);    i += 32; s_have_hub_pub = true;
    memcpy(s_stream_pub, d + i, 32); i += 32; s_have_stream_pub = true;

    if (i + 1 > n) goto malformed;
    uint8_t hl = d[i++];
    if (i + hl + 2 > n) goto malformed;
    size_t hc = hl < sizeof s_hub_host - 1 ? hl : sizeof s_hub_host - 1;
    memcpy(s_hub_host, d + i, hc); s_hub_host[hc] = '\0'; i += hl;
    s_hub_port = (uint16_t)(d[i] << 8 | d[i + 1]); i += 2;
    s_have_hub_ep = true;

    if (i + 1 > n) goto malformed;
    uint8_t sl = d[i++];
    if (i + sl + 2 > n) goto malformed;
    char stream_host[40];
    size_t sc = sl < sizeof stream_host - 1 ? sl : sizeof stream_host - 1;
    memcpy(stream_host, d + i, sc); stream_host[sc] = '\0'; i += sl;
    uint16_t stream_port = (uint16_t)(d[i] << 8 | d[i + 1]); i += 2;

    /* Optional trailing TLVs (version stays 1: pre-TLV firmware simply
     * ignored trailing bytes, so appending is backward-compatible).
     *   'G' 0x47  [u16be len][json]  gateway identity blob
     * The gateway blob is stored DORMANT: on hardware that cannot be a
     * gateway (every ESP camera today) it just sits in NVS until an NCP
     * and the gateway role exist — per the camera-as-gateway design,
     * provisioning always delivers it so enabling later needs no
     * re-provision. */
    while (i + 3 <= n) {
        uint8_t  t   = d[i];
        uint16_t tl  = (uint16_t)(d[i + 1] << 8 | d[i + 2]);
        i += 3;
        if (i + tl > n) break;               /* truncated TLV: stop */
        if (t == 0x47 && tl > 0 && tl <= 512) {
            if (nn_osal_kv_save(KV_KEY("gw_blob"), d + i, tl) == 0) {
                NN_LOG_INF("gateway data stored (%uB, dormant: no gateway "
                           "role on this hardware)", (unsigned)tl);
            }
        }
        i += tl;
    }

    /* Apply: hub pub feeds the crypto (needed to decrypt the WIFI write next);
     * stream endpoint goes straight into nn_netstream. */
    nn_prov_crypto_set_hub_pub(s_hub_pub);
    nn_netstream_set_host(stream_host, stream_port);
    nn_netstream_set_stream_key(s_stream_pub);   /* encrypt the uplink */
    cfg_save_config();

    NN_LOG_INF("CONFIG applied: name='%s' hub=%s:%u stream=%s:%u",
             s_name, s_hub_host, s_hub_port, stream_host, stream_port);
    set_status(NN_PROV_APPLYING);
    return ESP_OK;

malformed:
    NN_LOG_ERR("CONFIG: truncated (%u bytes)", (unsigned)n);
    set_status(NN_PROV_ERROR);
    return ESP_ERR_INVALID_SIZE;
}

esp_err_t nn_prov_handle_wifi(const uint8_t *d, size_t n)
{
    if (!nn_prov_crypto_ready()) {
        NN_LOG_ERR("WIFI before CONFIG (no hub key) — rejecting");
        set_status(NN_PROV_ERROR);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t plain[128];
    size_t plen = sizeof plain;
    esp_err_t r = nn_prov_crypto_decrypt_h2d((const char *)d, n, plain, &plen);
    if (r != ESP_OK) { set_status(NN_PROV_ERROR); return r; }

    /* plaintext: [ssid_len][ssid][pass_len][pass] */
    size_t i = 0;
    if (plen < 1) goto bad;
    uint8_t sl = plain[i++];
    if (i + sl + 1 > plen) goto bad;
    char ssid[33], pass[65];
    size_t sc = sl < sizeof ssid - 1 ? sl : sizeof ssid - 1;
    memcpy(ssid, plain + i, sc); ssid[sc] = '\0'; i += sl;
    uint8_t pl = plain[i++];
    if (i + pl > plen) goto bad;
    size_t pc = pl < sizeof pass - 1 ? pl : sizeof pass - 1;
    memcpy(pass, plain + i, pc); pass[pc] = '\0';

    nn_netstream_set_wifi(ssid, pass);
    memset(plain, 0, sizeof plain);
    memset(pass, 0, sizeof pass);
    cfg_mark_done();

    NN_LOG_INF("WIFI applied: ssid='%s' — provisioned", ssid);
    /* Notify SUCCESS *before* touching Wi-Fi: bringing the radio up here would
     * starve the BLE notify via coex (the reason the provisioner never saw
     * SUCCESS pre-fix).  Do NOT start the uplink live either — that leaves the
     * control channel dormant (nn_ctrl_start is boot-path only).  Instead reboot
     * into the clean provisioned path, which starts uplink + nn_ctrl and drops
     * BLE. */
    set_status(NN_PROV_SUCCESS);
    {
        /* Static storage: the thread outlives this call, and on the
         * bare-metal backends there is no heap to allocate a stack from. */
        static NN_OSAL_THREAD_STACK_DEFINE(reboot_stack, 2048);
        static nn_osal_thread_t reboot_thread;
        int trc = nn_osal_thread_create(&reboot_thread, reboot_stack,
                                        sizeof reboot_stack,
                                        prov_reboot_task, NULL, NULL, NULL,
                                        5, "prov_reboot");
        if (trc) {
            /* Never silently stay un-rebooted: the device would sit in the
             * unprovisioned runtime with no control channel, looking
             * provisioned to the wizard. */
            NN_LOG_ERR("reboot thread: %d — rebooting inline", trc);
            nn_osal_sleep_ms(1500);
            nn_osal_sys_reboot(NN_OSAL_REBOOT_COLD);
        }
    }
    return ESP_OK;

bad:
    memset(plain, 0, sizeof plain);
    NN_LOG_ERR("WIFI: bad plaintext layout");
    set_status(NN_PROV_ERROR);
    return ESP_ERR_INVALID_SIZE;
}

/* After a successful *live* provisioning the device is still in its
 * unprovisioned runtime: BLE is advertising and there is NO control channel,
 * because node_mgr_start wires nn_ctrl (+ drops BLE) only on the *provisioned
 * boot* path.  Reboot so the next boot takes that path.  Deferred ~1.5s so the
 * SUCCESS status notify and the WIFI write-response flush over BLE first. */
static void prov_reboot_task(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    nn_osal_sleep_ms(1500);
    NN_LOG_WRN("provisioned — rebooting into Wi-Fi uplink + control channel");
    nn_osal_sys_reboot(NN_OSAL_REBOOT_COLD);
}

nn_prov_status_t nn_prov_current_status(void) { return s_status; }

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t nn_prov_init(void)
{
    esp_err_t r = nn_prov_crypto_init();
    if (r != ESP_OK) return r;

    cfg_load();
    if (s_have_hub_pub) nn_prov_crypto_set_hub_pub(s_hub_pub);  /* restore for control/stream crypto */
    if (s_have_stream_pub) nn_netstream_set_stream_key(s_stream_pub);  /* encrypt uplink after reboot */
    if (s_provisioned) {
        s_status = NN_PROV_SUCCESS;
        NN_LOG_INF("restored provisioning: name='%s' hub=%s:%u",
                 s_name[0] ? s_name : "(unset)", s_hub_host, s_hub_port);
    } else {
        NN_LOG_INF("not provisioned yet — advertise over BLE to provision");
    }
    return ESP_OK;
}

esp_err_t nn_prov_start(void) { return nn_prov_ble_start(); }

bool nn_prov_is_provisioned(void) { return s_provisioned; }

void nn_prov_get_device_pubkey(uint8_t out[32]) { nn_prov_crypto_device_pub(out); }

bool nn_prov_get_hub_endpoint(char *host, size_t cap, uint16_t *port)
{
    if (!s_have_hub_ep) return false;
    if (host && cap) strlcpy(host, s_hub_host, cap);
    if (port) *port = s_hub_port;
    return true;
}

bool nn_prov_get_hub_pubkey(uint8_t out[32])
{
    if (!s_have_hub_pub) return false;
    memcpy(out, s_hub_pub, 32);
    return true;
}

bool nn_prov_get_stream_pubkey(uint8_t out[32])
{
    if (!s_have_stream_pub) return false;
    memcpy(out, s_stream_pub, 32);
    return true;
}

void nn_prov_status_str(char *out, size_t n)
{
    static const char *names[] = { "idle", "applying", "success", "error" };
    uint8_t dp[32]; nn_prov_crypto_device_pub(dp);
    snprintf(out, n, "status=%s provisioned=%s name='%s' hub=%s:%u hub_key=%s stream_key=%s devpub=%02x%02x%02x%02x..",
             names[s_status & 3], s_provisioned ? "yes" : "no",
             s_name[0] ? s_name : "(unset)",
             s_have_hub_ep ? s_hub_host : "(unset)", s_hub_port,
             s_have_hub_pub ? "yes" : "no", s_have_stream_pub ? "yes" : "no",
             dp[0], dp[1], dp[2], dp[3]);
}
