/* SPDX-License-Identifier: Apache-2.0 */
/* nn_crypto — converged ECIES v2 over PSA + nn_osal.  See nn_crypto.h. */
#include "nn_crypto/nn_crypto.h"
#include "nn_osal/log.h"
#include "nn_osal/sync.h"
#include "nn_osal/storage.h"

#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <psa/crypto.h>

NN_OSAL_LOG_MODULE("nn_crypto");

static const uint8_t INFO_H2D[] = "nn-hub-v2-h2d";
static const uint8_t INFO_D2H[] = "nn-hub-v2-d2h";
#define INFO_H2D_LEN (sizeof(INFO_H2D) - 1)
#define INFO_D2H_LEN (sizeof(INFO_D2H) - 1)

#define KV_PREFIX  "nn_crypto"
#define KV_DEV     "dev_x25519_priv"
#define KV_HUB     "hub_x25519_pub"

static uint8_t s_dev_priv[32];
static uint8_t s_dev_pub[32];
static uint8_t s_hub_pub[32];
static bool    s_have_saved, s_dev_ready, s_hub_ready;
static nn_osal_mutex_t s_lock;
static bool    s_lock_init;

static void ensure_lock(void)
{
    if (!s_lock_init) { nn_osal_mutex_init(&s_lock); s_lock_init = true; }
}

/* ── bundled base64 (RFC 4648) so we don't depend on a platform mbedtls ─── */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_decode(const char *in, size_t in_len, uint8_t *out, size_t cap, size_t *out_len)
{
    static int8_t rev[256];
    static bool init;
    if (!init) {
        for (int i = 0; i < 256; i++) rev[i] = -1;
        for (int i = 0; i < 64; i++)  rev[(uint8_t)B64[i]] = (int8_t)i;
        init = true;
    }
    uint32_t acc = 0; int bits = 0; size_t n = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int8_t v = rev[(uint8_t)c];
        if (v < 0) return -EINVAL;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; if (n >= cap) return -ENOSPC; out[n++] = (uint8_t)(acc >> bits); }
    }
    *out_len = n; return 0;
}

static int b64_encode(const uint8_t *in, size_t in_len, char *out, size_t cap)
{
    size_t need = ((in_len + 2) / 3) * 4 + 1;
    if (cap < need) return -ENOSPC;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = in[i] << 16;
        if (i + 1 < in_len) v |= in[i + 1] << 8;
        if (i + 2 < in_len) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 0x3f];
        out[o++] = B64[(v >> 12) & 0x3f];
        out[o++] = (i + 1 < in_len) ? B64[(v >> 6) & 0x3f] : '=';
        out[o++] = (i + 2 < in_len) ? B64[v & 0x3f] : '=';
    }
    out[o] = '\0'; return 0;
}

/* ── minimal flat-JSON value extractor (envelope is hub-generated, compact) ─ */
static const char *json_find(const char *json, size_t len, const char *key, size_t *vlen)
{
    char pat[16]; int pl = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (pl <= 0 || (size_t)pl >= sizeof pat) return NULL;
    for (size_t i = 0; i + (size_t)pl < len; i++) {
        if (memcmp(json + i, pat, pl) != 0) continue;
        size_t j = i + pl;
        while (j < len && json[j] != ':') j++;
        j++;
        while (j < len && (json[j] == ' ' || json[j] == '"')) { if (json[j] == '"') { j++; break; } j++; }
        size_t start = j;
        while (j < len && json[j] != '"' && json[j] != ',' && json[j] != '}') j++;
        *vlen = j - start; return json + start;
    }
    return NULL;
}

/* ── PSA helpers ─────────────────────────────────────────────────────────── */
static psa_status_t import_x25519_priv(const uint8_t priv[32], psa_key_id_t *out)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&a, 255);
    psa_set_key_algorithm(&a, PSA_ALG_ECDH);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
    return psa_import_key(&a, priv, 32, out);
}

static psa_status_t derive_aes_key(const uint8_t *ikm, size_t ikm_len, const uint8_t epk[32],
                                   const uint8_t *info, size_t info_len, psa_key_id_t *out)
{
    psa_key_id_t ikm_id = PSA_KEY_ID_NULL;
    psa_key_attributes_t h = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&h, PSA_KEY_TYPE_DERIVE);
    psa_set_key_algorithm(&h, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&h, PSA_KEY_USAGE_DERIVE);
    psa_status_t rc = psa_import_key(&h, ikm, ikm_len, &ikm_id);
    if (rc != PSA_SUCCESS) return rc;
    psa_key_derivation_operation_t kdf = PSA_KEY_DERIVATION_OPERATION_INIT;
    rc = psa_key_derivation_setup(&kdf, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    if (rc != PSA_SUCCESS) goto out_key;
    rc = psa_key_derivation_input_bytes(&kdf, PSA_KEY_DERIVATION_INPUT_SALT, epk, 32);
    if (rc != PSA_SUCCESS) goto out_op;
    rc = psa_key_derivation_input_key(&kdf, PSA_KEY_DERIVATION_INPUT_SECRET, ikm_id);
    if (rc != PSA_SUCCESS) goto out_op;
    rc = psa_key_derivation_input_bytes(&kdf, PSA_KEY_DERIVATION_INPUT_INFO, info, info_len);
    if (rc != PSA_SUCCESS) goto out_op;
    psa_key_attributes_t aa = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&aa, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&aa, 256);
    psa_set_key_algorithm(&aa, PSA_ALG_GCM);
    psa_set_key_usage_flags(&aa, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    rc = psa_key_derivation_output_key(&aa, &kdf, out);
out_op:
    psa_key_derivation_abort(&kdf);
out_key:
    psa_destroy_key(ikm_id);
    return rc;
}

/* ── key lifecycle ───────────────────────────────────────────────────────── */
static int kv_cb(const char *suffix, const uint8_t *v, size_t n, void *u)
{
    (void)u;
    if (!strcmp(suffix, KV_DEV) && n == 32) { memcpy(s_dev_priv, v, 32); s_have_saved = true; }
    else if (!strcmp(suffix, KV_HUB) && n == 32) { memcpy(s_hub_pub, v, 32); s_hub_ready = true; }
    return 0;
}

int nn_crypto_init(void)
{
    ensure_lock();
    nn_osal_kv_init();
    nn_osal_kv_register(KV_PREFIX, kv_cb, NULL);
    nn_osal_kv_load_all();

    if (psa_crypto_init() != PSA_SUCCESS) { NN_LOG_ERR("psa_crypto_init failed"); return -EIO; }

    if (s_have_saved) {
        psa_key_id_t id = PSA_KEY_ID_NULL;
        if (import_x25519_priv(s_dev_priv, &id) != PSA_SUCCESS) return -EIO;
        size_t pl;
        psa_status_t e = psa_export_public_key(id, s_dev_pub, 32, &pl);
        psa_destroy_key(id);
        if (e != PSA_SUCCESS || pl != 32) return -EIO;
    } else {
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
        psa_set_key_bits(&a, 255);
        psa_set_key_algorithm(&a, PSA_ALG_ECDH);
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
        psa_key_id_t id = PSA_KEY_ID_NULL;
        if (psa_generate_key(&a, &id) != PSA_SUCCESS) return -EIO;
        size_t l;
        if (psa_export_key(id, s_dev_priv, 32, &l) != PSA_SUCCESS || l != 32 ||
            psa_export_public_key(id, s_dev_pub, 32, &l) != PSA_SUCCESS || l != 32) {
            psa_destroy_key(id); return -EIO;
        }
        psa_destroy_key(id);
        nn_osal_kv_save(KV_PREFIX "/" KV_DEV, s_dev_priv, 32);
        s_have_saved = true;
    }
    s_dev_ready = true;
    NN_LOG_INF("device X25519 ready (pub %02x%02x%02x%02x..)",
               s_dev_pub[0], s_dev_pub[1], s_dev_pub[2], s_dev_pub[3]);
    return 0;
}

void nn_crypto_device_pub(uint8_t out[32]) { memcpy(out, s_dev_pub, 32); }

int nn_crypto_set_hub_pub(const uint8_t hub_pub[32])
{
    memcpy(s_hub_pub, hub_pub, 32); s_hub_ready = true;
    nn_osal_kv_save(KV_PREFIX "/" KV_HUB, s_hub_pub, 32);
    return 0;
}

bool nn_crypto_ready(void) { return s_dev_ready && s_hub_ready; }

int nn_crypto_test_set_device_priv(const uint8_t priv[32])
{
    ensure_lock();
    memcpy(s_dev_priv, priv, 32);
    psa_crypto_init();
    psa_key_id_t id = PSA_KEY_ID_NULL;
    if (import_x25519_priv(priv, &id) != PSA_SUCCESS) return -EIO;
    size_t pl; psa_export_public_key(id, s_dev_pub, 32, &pl); psa_destroy_key(id);
    s_have_saved = s_dev_ready = true;
    return 0;
}

/* ── primitives ──────────────────────────────────────────────────────────── */
static int nn_crypto_gen_x25519_unlocked(uint8_t pub[32], uint8_t priv[32])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&a, 255);
    psa_set_key_algorithm(&a, PSA_ALG_ECDH);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
    psa_key_id_t id = PSA_KEY_ID_NULL;
    if (psa_generate_key(&a, &id) != PSA_SUCCESS) return -EIO;
    size_t n; int r = -EIO;
    if (psa_export_key(id, priv, 32, &n) == PSA_SUCCESS && n == 32 &&
        psa_export_public_key(id, pub, 32, &n) == PSA_SUCCESS && n == 32) r = 0;
    psa_destroy_key(id); return r;
}

static int nn_crypto_x25519_unlocked(const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32])
{
    psa_key_id_t id = PSA_KEY_ID_NULL;
    if (import_x25519_priv(priv, &id) != PSA_SUCCESS) return -EIO;
    size_t n; psa_status_t e = psa_raw_key_agreement(PSA_ALG_ECDH, id, peer, 32, out, 32, &n);
    psa_destroy_key(id);
    return (e == PSA_SUCCESS && n == 32) ? 0 : -EIO;
}

int nn_crypto_device_x25519(const uint8_t peer[32], uint8_t out[32])
{
    if (!s_dev_ready) return -EACCES;
    ensure_lock();
    nn_osal_mutex_lock(&s_lock, NN_OSAL_WAIT_FOREVER);
    int r = nn_crypto_x25519_unlocked(s_dev_priv, peer, out);
    nn_osal_mutex_unlock(&s_lock);
    return r;
}

static int nn_crypto_hkdf_unlocked(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                   const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len)
{
    psa_key_id_t ikm_id = PSA_KEY_ID_NULL;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_DERIVE);
    psa_set_key_algorithm(&a, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
    if (psa_import_key(&a, ikm, ikm_len, &ikm_id) != PSA_SUCCESS) return -EIO;
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    int r = -EIO;
    if (psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256)) != PSA_SUCCESS) goto out;
    if (psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len) != PSA_SUCCESS) goto ab;
    if (psa_key_derivation_input_key(&op, PSA_KEY_DERIVATION_INPUT_SECRET, ikm_id) != PSA_SUCCESS) goto ab;
    if (psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, info, info_len) != PSA_SUCCESS) goto ab;
    if (psa_key_derivation_output_bytes(&op, out, out_len) == PSA_SUCCESS) r = 0;
ab:
    psa_key_derivation_abort(&op);
out:
    psa_destroy_key(ikm_id);
    return r;
}

static int nn_crypto_aesgcm_unlocked(bool decrypt, const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t in_len,
                     uint8_t *out, size_t out_cap, size_t *out_len)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 256);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    psa_set_key_usage_flags(&a, decrypt ? PSA_KEY_USAGE_DECRYPT : PSA_KEY_USAGE_ENCRYPT);
    psa_key_id_t id = PSA_KEY_ID_NULL;
    if (psa_import_key(&a, key, 32, &id) != PSA_SUCCESS) return -EIO;
    psa_status_t e = decrypt
        ? psa_aead_decrypt(id, PSA_ALG_GCM, nonce, 12, aad, aad_len, in, in_len, out, out_cap, out_len)
        : psa_aead_encrypt(id, PSA_ALG_GCM, nonce, 12, aad, aad_len, in, in_len, out, out_cap, out_len);
    psa_destroy_key(id);
    return e == PSA_SUCCESS ? 0 : -EIO;
}

/* ── ECIES v2 ────────────────────────────────────────────────────────────── */
static int decrypt_h2d_locked(const char *json, size_t json_len, uint8_t *out, size_t *out_len)
{
    if (!s_dev_ready) return -EACCES;
    if (!s_hub_ready) return -EACCES;
    size_t vl;
    const char *vv = json_find(json, json_len, "v", &vl);
    if (!vv || vl < 1 || vv[0] != '2') return -EBADMSG;

    uint8_t epk[32], nonce[12], ct[512];
    size_t el = 0, nl = 0, cl = 0;
    const char *e = json_find(json, json_len, "epk", &el);
    const char *n = json_find(json, json_len, "nonce", &nl);
    const char *c = json_find(json, json_len, "ct", &cl);
    if (!e || !n || !c) return -EBADMSG;
    size_t bl;
    if (b64_decode(e, el, epk, 32, &bl) || bl != 32) return -EINVAL;
    if (b64_decode(n, nl, nonce, 12, &bl) || bl != 12) return -EINVAL;
    if (b64_decode(c, cl, ct, sizeof ct, &cl)) return -EINVAL;

    uint8_t ikm[64]; size_t sh;
    psa_key_id_t dev = PSA_KEY_ID_NULL;
    if (import_x25519_priv(s_dev_priv, &dev) != PSA_SUCCESS) return -EIO;
    psa_status_t prc = psa_raw_key_agreement(PSA_ALG_ECDH, dev, epk, 32, ikm, 32, &sh);
    if (prc == PSA_SUCCESS && sh == 32)
        prc = psa_raw_key_agreement(PSA_ALG_ECDH, dev, s_hub_pub, 32, ikm + 32, 32, &sh);
    psa_destroy_key(dev);
    if (prc != PSA_SUCCESS || sh != 32) return -EIO;

    psa_key_id_t aes = PSA_KEY_ID_NULL;
    prc = derive_aes_key(ikm, 64, epk, INFO_H2D, INFO_H2D_LEN, &aes);
    memset(ikm, 0, sizeof ikm);
    if (prc != PSA_SUCCESS) return -EIO;
    prc = psa_aead_decrypt(aes, PSA_ALG_GCM, nonce, 12, NULL, 0, ct, cl, out, *out_len, out_len);
    psa_destroy_key(aes);
    return prc == PSA_SUCCESS ? 0 : -EACCES;
}

int nn_crypto_decrypt_h2d(const char *json, size_t json_len, uint8_t *out, size_t *out_len)
{
    ensure_lock();
    nn_osal_mutex_lock(&s_lock, NN_OSAL_WAIT_FOREVER);
    int r = decrypt_h2d_locked(json, json_len, out, out_len);
    nn_osal_mutex_unlock(&s_lock);
    return r;
}

static int encrypt_d2h_locked(const uint8_t *plain, size_t plain_len, char *json_out, size_t cap)
{
    if (!s_dev_ready || !s_hub_ready) return -EACCES;
    uint8_t epk[32], epk_priv[32];
    /* UNLOCKED inner: this function already runs under s_lock, and the
     * osal mutex is not recursive — calling the locking wrapper here
     * self-deadlocks.  That was found the hard way: the first sealed
     * report after the psa-lock patch hung the send path for 5 h
     * (up=0, frames frozen, capture alive) on 2026-08-19. */
    if (nn_crypto_gen_x25519_unlocked(epk, epk_priv) != 0) return -EIO;

    uint8_t ikm[64]; size_t sh;
    psa_key_id_t e2 = PSA_KEY_ID_NULL, st = PSA_KEY_ID_NULL;
    if (import_x25519_priv(epk_priv, &e2) != PSA_SUCCESS) return -EIO;
    if (import_x25519_priv(s_dev_priv, &st) != PSA_SUCCESS) { psa_destroy_key(e2); return -EIO; }
    psa_status_t prc = psa_raw_key_agreement(PSA_ALG_ECDH, e2, s_hub_pub, 32, ikm, 32, &sh);
    if (prc == PSA_SUCCESS && sh == 32)
        prc = psa_raw_key_agreement(PSA_ALG_ECDH, st, s_hub_pub, 32, ikm + 32, 32, &sh);
    psa_destroy_key(e2); psa_destroy_key(st);
    memset(epk_priv, 0, sizeof epk_priv);
    if (prc != PSA_SUCCESS || sh != 32) return -EIO;

    psa_key_id_t aes = PSA_KEY_ID_NULL;
    prc = derive_aes_key(ikm, 64, epk, INFO_D2H, INFO_D2H_LEN, &aes);
    memset(ikm, 0, sizeof ikm);
    if (prc != PSA_SUCCESS) return -EIO;
    uint8_t nonce[12];
    if (psa_generate_random(nonce, 12) != PSA_SUCCESS) { psa_destroy_key(aes); return -EIO; }
    uint8_t ctbuf[288]; size_t ctlen;
    prc = psa_aead_encrypt(aes, PSA_ALG_GCM, nonce, 12, NULL, 0, plain, plain_len,
                           ctbuf, sizeof ctbuf, &ctlen);
    psa_destroy_key(aes);
    if (prc != PSA_SUCCESS) return -EIO;

    char b_epk[48], b_non[20], b_ct[400];
    if (b64_encode(epk, 32, b_epk, sizeof b_epk) || b64_encode(nonce, 12, b_non, sizeof b_non) ||
        b64_encode(ctbuf, ctlen, b_ct, sizeof b_ct)) return -ENOSPC;
    int w = snprintf(json_out, cap, "{\"v\":2,\"epk\":\"%s\",\"nonce\":\"%s\",\"ct\":\"%s\"}",
                     b_epk, b_non, b_ct);
    return (w > 0 && (size_t)w < cap) ? 0 : -ENOSPC;
}

int nn_crypto_encrypt_d2h(const uint8_t *plain, size_t plain_len, char *json_out, size_t cap)
{
    ensure_lock();
    nn_osal_mutex_lock(&s_lock, NN_OSAL_WAIT_FOREVER);
    int r = encrypt_d2h_locked(plain, plain_len, json_out, cap);
    nn_osal_mutex_unlock(&s_lock);
    return r;
}


/* ── PSA serialization wrappers ──────────────────────────────────────────────
 * mbedtls PSA is NOT thread-safe: its key-slot table has no internal
 * locking, so two threads importing/destroying keys concurrently corrupt
 * it and a later psa_import_key_into_slot memcpys through a wild slot
 * pointer.  That is exactly the 2026-08-18 cam3 crash: the video path
 * (appsink on_sample -> nn_sectun_send -> nn_crypto_aesgcm, one key
 * import PER PACKET) raced a reconnect handshake on another thread and
 * SIGSEGV'd in psa_import_key_into_slot — and the dying process left the
 * C7x wedged until reboot.  decrypt_h2d/encrypt_d2h already took s_lock;
 * these four entry points went bare.  Every public PSA-touching function
 * now serializes on the same s_lock.
 */
int nn_crypto_gen_x25519(uint8_t pub[32], uint8_t priv[32])
{
    ensure_lock();
    nn_osal_mutex_lock(&s_lock, NN_OSAL_WAIT_FOREVER);
    int r = nn_crypto_gen_x25519_unlocked(pub, priv);
    nn_osal_mutex_unlock(&s_lock);
    return r;
}

int nn_crypto_x25519(const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32])
{
    ensure_lock();
    nn_osal_mutex_lock(&s_lock, NN_OSAL_WAIT_FOREVER);
    int r = nn_crypto_x25519_unlocked(priv, peer, out);
    nn_osal_mutex_unlock(&s_lock);
    return r;
}

int nn_crypto_hkdf(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                   const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len)
{
    ensure_lock();
    nn_osal_mutex_lock(&s_lock, NN_OSAL_WAIT_FOREVER);
    int r = nn_crypto_hkdf_unlocked(ikm, ikm_len, salt, salt_len, info, info_len, out, out_len);
    nn_osal_mutex_unlock(&s_lock);
    return r;
}

int nn_crypto_aesgcm(bool decrypt, const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t in_len,
                     uint8_t *out, size_t out_cap, size_t *out_len)
{
    ensure_lock();
    nn_osal_mutex_lock(&s_lock, NN_OSAL_WAIT_FOREVER);
    int r = nn_crypto_aesgcm_unlocked(decrypt, key, nonce, aad, aad_len,
                                      in, in_len, out, out_cap, out_len);
    nn_osal_mutex_unlock(&s_lock);
    return r;
}
