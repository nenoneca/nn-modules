/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/storage.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#ifdef CONFIG_IMG_BLOCK_BUF
#  include <zephyr/dfu/flash_img.h>
#endif
#ifdef CONFIG_BOOTLOADER_MCUBOOT
#  include <zephyr/dfu/mcuboot.h>
#endif
#include <errno.h>
#include <string.h>

/* ── key-value (Zephyr settings backend) ───────────────────────── */

#define MAX_REGISTRATIONS  8

struct kv_reg {
    char prefix[32];
    size_t prefix_len;
    nn_osal_kv_load_cb cb;
    void *user;
    /* Zephyr settings_handler stitched in at registration. */
    struct settings_handler h;
};
static struct kv_reg s_regs[MAX_REGISTRATIONS];
static unsigned       s_reg_count;
static bool           s_inited;

/* Largest single value we can load back.  Auto-engine rule blobs are
 * the biggest nn_osal_kv value (up to ~768 B); the prior 256 B cap
 * silently rejected anything larger with -ENOMEM. */
#define KV_VALUE_MAX  1024

/* Dispatch a loaded value to registration `idx`.  Zephyr strips the
 * handler's name from the key, so `suffix` is already relative to the
 * registered prefix (e.g. key "auto/blob" with handler name "auto"
 * arrives here as suffix="blob").  We therefore do NOT re-match the
 * prefix — the per-slot thunk already told us which registration this
 * is. */
static int kv_dispatch(unsigned idx, const char *suffix, size_t len,
                       settings_read_cb read_cb, void *cb_arg)
{
    if (idx >= s_reg_count) return -ENOENT;
    struct kv_reg *r = &s_regs[idx];
    uint8_t buf[KV_VALUE_MAX];
    if (len > sizeof buf) return -ENOMEM;
    ssize_t n = read_cb(cb_arg, buf, len);
    if (n < 0) return n;
    return r->cb(suffix, buf, (size_t)n, r->user);
}

/* Zephyr's settings_handler.h_set carries no user context, so a single
 * shared thunk can't tell which registration fired it.  Define one tiny
 * thunk per slot, each binding its index, and hand the matching thunk to
 * settings_register().  (The previous shared thunk compared the already-
 * stripped suffix against the full prefix and so never matched — every
 * load returned -ENOENT, silently defeating all nn_osal_kv persistence.) */
#define KV_THUNK(n)                                                       \
    static int kv_set_thunk_##n(const char *suffix, size_t len,          \
                                settings_read_cb rc, void *ca)           \
    { return kv_dispatch(n, suffix, len, rc, ca); }
KV_THUNK(0) KV_THUNK(1) KV_THUNK(2) KV_THUNK(3)
KV_THUNK(4) KV_THUNK(5) KV_THUNK(6) KV_THUNK(7)

static int (*const kv_set_thunks[MAX_REGISTRATIONS])(
        const char *, size_t, settings_read_cb, void *) = {
    kv_set_thunk_0, kv_set_thunk_1, kv_set_thunk_2, kv_set_thunk_3,
    kv_set_thunk_4, kv_set_thunk_5, kv_set_thunk_6, kv_set_thunk_7,
};

int nn_osal_kv_init(void)
{
    if (s_inited) return 0;
    int rv = settings_subsys_init();
    if (rv) return rv;
    s_inited = true;
    return 0;
}

int nn_osal_kv_register(const char *prefix, nn_osal_kv_load_cb cb,
                        void *user)
{
    if (!prefix || !cb) return -EINVAL;
    if (s_reg_count >= MAX_REGISTRATIONS) return -ENOMEM;
    struct kv_reg *r = &s_regs[s_reg_count];
    size_t pl = strlen(prefix);
    if (pl >= sizeof r->prefix) return -ENOMEM;
    memcpy(r->prefix, prefix, pl + 1);
    r->prefix_len = pl;
    r->cb = cb; r->user = user;
    r->h = (struct settings_handler){ .name = r->prefix,
                                       .h_set = kv_set_thunks[s_reg_count] };
    int rv = settings_register(&r->h);
    if (rv) return rv;
    s_reg_count++;
    /* Zephyr's `settings_register` only wires the handler for FUTURE
     * loads — it doesn't replay existing NVS contents.  Without this
     * subtree load, callers that nn_osal_kv_register *after*
     * settings_load() ran would never see any persisted values.  Load
     * the subtree now so registration is symmetric regardless of
     * timing. */
    (void)settings_load_subtree(r->prefix);
    return 0;
}

int nn_osal_kv_load_all(void) { return settings_load(); }

int nn_osal_kv_save(const char *key, const void *data, size_t len)
{
    return settings_save_one(key, data, len);
}

int nn_osal_kv_delete(const char *key)
{
    return settings_delete(key);
}

/* ── DFU ──────────────────────────────────────────────────────── */

/* DFU is only compiled if the app enables both IMG_BLOCK_BUF (for the
 * flash_img streaming API) and BOOTLOADER_MCUBOOT (for the swap-request
 * API).  Apps that don't OTA leave both off; the OSAL stubs then return
 * -ENOSYS so callers can detect the absence at runtime. */

#if defined(CONFIG_IMG_BLOCK_BUF) && defined(CONFIG_BOOTLOADER_MCUBOOT)

struct nn_osal_dfu_ctx {
    struct flash_img_context inner;
};

nn_osal_dfu_ctx_t *nn_osal_dfu_ctx_alloc(void)
{
    return k_malloc(sizeof(struct nn_osal_dfu_ctx));
}

void nn_osal_dfu_ctx_free(nn_osal_dfu_ctx_t *ctx) { k_free(ctx); }

int nn_osal_dfu_begin(nn_osal_dfu_ctx_t *ctx, size_t expected_size)
{
    (void)expected_size;
    if (!ctx) return -EINVAL;
    return flash_img_init(&ctx->inner);
}

int nn_osal_dfu_write(nn_osal_dfu_ctx_t *ctx,
                      const uint8_t *buf, size_t len)
{
    if (!ctx || !buf) return -EINVAL;
    return flash_img_buffered_write(&ctx->inner, buf, len, false);
}

int nn_osal_dfu_finalise(nn_osal_dfu_ctx_t *ctx)
{
    if (!ctx) return -EINVAL;
    return flash_img_buffered_write(&ctx->inner, NULL, 0, true);
}

int nn_osal_dfu_request_upgrade(bool permanent)
{
    return boot_request_upgrade(permanent ? BOOT_UPGRADE_PERMANENT
                                          : BOOT_UPGRADE_TEST);
}

int nn_osal_dfu_confirm(void) { return boot_write_img_confirmed(); }

#else  /* DFU stack not present */

nn_osal_dfu_ctx_t *nn_osal_dfu_ctx_alloc(void) { return NULL; }
void               nn_osal_dfu_ctx_free(nn_osal_dfu_ctx_t *c) { (void)c; }
int  nn_osal_dfu_begin(nn_osal_dfu_ctx_t *c, size_t s)         { (void)c; (void)s; return -ENOSYS; }
int  nn_osal_dfu_write(nn_osal_dfu_ctx_t *c, const uint8_t *b, size_t n) { (void)c; (void)b; (void)n; return -ENOSYS; }
int  nn_osal_dfu_finalise(nn_osal_dfu_ctx_t *c)                { (void)c; return -ENOSYS; }
int  nn_osal_dfu_request_upgrade(bool p)                       { (void)p; return -ENOSYS; }
int  nn_osal_dfu_confirm(void)                                 { return -ENOSYS; }

#endif
