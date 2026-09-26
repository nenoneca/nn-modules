/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal storage backend — ESP-IDF.
 *   - key-value: NVS.  Keys are "<namespace>/<subkey>" — the part before the
 *     first '/' is the NVS namespace (= the registered prefix), the rest is
 *     the NVS key.  load_all() iterates a namespace and dispatches subkeys.
 *   - DFU: stubbed (-ENOSYS) for slice 1 — OTA staging comes later. */
#include "nn_osal/storage.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* ── key-value (NVS) ─────────────────────────────────────────────────── */

#define KV_MAX_REG  8
static struct { char prefix[16]; nn_osal_kv_load_cb cb; void *user; } s_reg[KV_MAX_REG];
static int s_nreg;

int nn_osal_kv_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    return e == ESP_OK ? 0 : -EIO;
}

int nn_osal_kv_register(const char *prefix, nn_osal_kv_load_cb cb, void *user)
{
    if (s_nreg >= KV_MAX_REG) return -ENOMEM;
    strlcpy(s_reg[s_nreg].prefix, prefix, sizeof s_reg[0].prefix);
    s_reg[s_nreg].cb = cb;
    s_reg[s_nreg].user = user;
    s_nreg++;
    return 0;
}

/* "ns/sub" → ns + pointer to sub.  Returns 0 or -EINVAL. */
static int split_key(const char *key, char *ns, size_t nscap, const char **sub)
{
    const char *slash = strchr(key, '/');
    if (!slash) return -EINVAL;
    size_t n = (size_t)(slash - key);
    if (n == 0 || n >= nscap) return -EINVAL;
    memcpy(ns, key, n);
    ns[n] = '\0';
    *sub = slash + 1;
    return 0;
}

int nn_osal_kv_save(const char *key, const void *data, size_t len)
{
    char ns[16]; const char *sub;
    if (split_key(key, ns, sizeof ns, &sub)) return -EINVAL;
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return -EIO;
    esp_err_t e = nvs_set_blob(h, sub, data, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -EIO;
}

int nn_osal_kv_delete(const char *key)
{
    char ns[16]; const char *sub;
    if (split_key(key, ns, sizeof ns, &sub)) return -EINVAL;
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return -EIO;
    esp_err_t e = nvs_erase_key(h, sub);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) ? 0 : -EIO;
}

/* Read one entry of ANY scalar/blob type into buf, returning its length.
 *
 * kv_save() only ever writes blobs, but firmware that predates a module's
 * port to kv wrote typed entries (nvs_set_str/_u8/_u16/...) under the very
 * same namespace and key.  Iterating NVS_TYPE_BLOB alone would skip those,
 * so an upgraded device would silently come up with none of its settings —
 * for nn_prov that means a provisioned camera dropping back into BLE setup
 * mode.  Surface every type instead; the caller sees raw bytes either way.
 * Strings are reported WITHOUT the NUL so they match a kv_save()d value. */
static int kv_read_any(nvs_handle_t h, const nvs_entry_info_t *info,
                       uint8_t *buf, size_t cap, size_t *out_len)
{
    size_t sz = cap;
    switch (info->type) {
    case NVS_TYPE_STR:
        if (nvs_get_str(h, info->key, (char *)buf, &sz) != ESP_OK) return -1;
        *out_len = sz ? sz - 1 : 0;            /* drop the NUL */
        return 0;
    /* Each width needs its OWN getter: NVS type-checks strictly, so
     * nvs_get_u8() on an i8 entry returns TYPE_MISMATCH.  Read into a
     * typed local and memcpy — buf has no alignment guarantee. */
    case NVS_TYPE_U8: {
        uint8_t v;
        if (nvs_get_u8(h, info->key, &v) != ESP_OK) return -1;
        if (cap < 1) return -1;
        memcpy(buf, &v, 1); *out_len = 1; return 0;
    }
    case NVS_TYPE_I8: {
        int8_t v;
        if (nvs_get_i8(h, info->key, &v) != ESP_OK) return -1;
        if (cap < 1) return -1;
        memcpy(buf, &v, 1); *out_len = 1; return 0;
    }
    case NVS_TYPE_U16: {
        uint16_t v;
        if (nvs_get_u16(h, info->key, &v) != ESP_OK) return -1;
        if (cap < sizeof v) return -1;
        memcpy(buf, &v, sizeof v); *out_len = sizeof v; return 0;
    }
    case NVS_TYPE_I16: {
        int16_t v;
        if (nvs_get_i16(h, info->key, &v) != ESP_OK) return -1;
        if (cap < sizeof v) return -1;
        memcpy(buf, &v, sizeof v); *out_len = sizeof v; return 0;
    }
    case NVS_TYPE_U32: {
        uint32_t v;
        if (nvs_get_u32(h, info->key, &v) != ESP_OK) return -1;
        if (cap < sizeof v) return -1;
        memcpy(buf, &v, sizeof v); *out_len = sizeof v; return 0;
    }
    case NVS_TYPE_I32: {
        int32_t v;
        if (nvs_get_i32(h, info->key, &v) != ESP_OK) return -1;
        if (cap < sizeof v) return -1;
        memcpy(buf, &v, sizeof v); *out_len = sizeof v; return 0;
    }
    default:                                   /* BLOB and anything else */
        sz = 0;
        if (nvs_get_blob(h, info->key, NULL, &sz) != ESP_OK || sz > cap) return -1;
        if (nvs_get_blob(h, info->key, buf, &sz) != ESP_OK) return -1;
        *out_len = sz;
        return 0;
    }
}

int nn_osal_kv_load_all(void)
{
    for (int i = 0; i < s_nreg; i++) {
        nvs_iterator_t it = NULL;
        esp_err_t r = nvs_entry_find("nvs", s_reg[i].prefix, NVS_TYPE_ANY, &it);
        while (r == ESP_OK) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            nvs_handle_t h;
            if (nvs_open(s_reg[i].prefix, NVS_READONLY, &h) == ESP_OK) {
                uint8_t buf[512];
                size_t  sz = 0;
                if (kv_read_any(h, &info, buf, sizeof buf, &sz) == 0)
                    s_reg[i].cb(info.key, buf, sz, s_reg[i].user);
                nvs_close(h);
            }
            r = nvs_entry_next(&it);
        }
        if (it) nvs_release_iterator(it);
    }
    return 0;
}

/* ── DFU (stubbed for slice 1) ───────────────────────────────────────── */

struct nn_osal_dfu_ctx { int unused; };

nn_osal_dfu_ctx_t *nn_osal_dfu_ctx_alloc(void) { return calloc(1, sizeof(struct nn_osal_dfu_ctx)); }
void               nn_osal_dfu_ctx_free(nn_osal_dfu_ctx_t *ctx) { free(ctx); }

int nn_osal_dfu_begin(nn_osal_dfu_ctx_t *c, size_t n)           { (void)c; (void)n; return -ENOSYS; }
int nn_osal_dfu_write(nn_osal_dfu_ctx_t *c, const uint8_t *b, size_t n) { (void)c; (void)b; (void)n; return -ENOSYS; }
int nn_osal_dfu_finalise(nn_osal_dfu_ctx_t *c)                  { (void)c; return -ENOSYS; }
int nn_osal_dfu_request_upgrade(bool permanent)                { (void)permanent; return -ENOSYS; }
int nn_osal_dfu_confirm(void)                                  { return -ENOSYS; }
