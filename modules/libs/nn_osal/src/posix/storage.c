/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal storage backend — POSIX.
 *   - key-value: one file per key under NN_OSAL_KV_DIR (default
 *     /var/lib/nn/kv, falling back to $HOME/.nn_kv when unwritable).
 *     Keys are "<prefix>/<subkey>", exactly the NVS-backend contract:
 *     the prefix is a subdirectory, load_all() walks each registered
 *     prefix dir and dispatches subkeys to the registered callback.
 *   - DFU: package upgrades are the distro/systemd's job on Linux
 *     (-ENOSYS, same as the ESP backend's slice-1 stubs). */
#include "nn_osal/storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#define KV_MAX_REG  8
static struct { char prefix[16]; nn_osal_kv_load_cb cb; void *user; } s_reg[KV_MAX_REG];
static int  s_nreg;
static char s_dir[256];

static const char *kv_dir(void)
{
    if (s_dir[0]) return s_dir;
    const char *e = getenv("NN_OSAL_KV_DIR");
    if (e && *e) { snprintf(s_dir, sizeof s_dir, "%s", e); return s_dir; }
    snprintf(s_dir, sizeof s_dir, "/var/lib/nn/kv");
    if (mkdir("/var/lib/nn", 0755) != 0 && errno != EEXIST) goto home;
    if (mkdir(s_dir, 0755) != 0 && errno != EEXIST) goto home;
    if (access(s_dir, W_OK) == 0) return s_dir;
home:
    /*
     * Falling back here SILENTLY is a trap: the caller gets an empty store
     * rather than an error, so a read-only or missing /var/lib/nn (a
     * container bind-mounted ro, say) looks exactly like a device that was
     * never provisioned.  Say so once, loudly.
     */
    snprintf(s_dir, sizeof s_dir, "%s/.nn_kv", getenv("HOME") ? getenv("HOME") : "/tmp");
    /* stderr, not NN_LOG: this runs during init, and the storage backend
     * should not drag a log backend into every target that links it (the
     * kv_dispatch host test links storage.c alone).  systemd captures
     * stderr to the journal anyway. */
    fprintf(stderr, "nn_osal_kv: WARNING /var/lib/nn/kv not writable — "
                    "falling back to %s; anything stored elsewhere will "
                    "look MISSING\n", s_dir);
    return s_dir;
}

int nn_osal_kv_init(void)
{
    const char *d = kv_dir();
    if (mkdir(d, 0700) != 0 && errno != EEXIST) return -EIO;
    return 0;
}

int nn_osal_kv_register(const char *prefix, nn_osal_kv_load_cb cb, void *user)
{
    if (s_nreg >= KV_MAX_REG) return -ENOMEM;
    snprintf(s_reg[s_nreg].prefix, sizeof s_reg[0].prefix, "%s", prefix);
    s_reg[s_nreg].cb = cb;
    s_reg[s_nreg].user = user;
    s_nreg++;
    return 0;
}

static int key_path(const char *key, char *out, size_t cap, bool mkparent)
{
    const char *slash = strchr(key, '/');
    if (!slash || slash == key || strchr(slash + 1, '/')) return -EINVAL;
    if (mkparent) {
        char dir[300];
        snprintf(dir, sizeof dir, "%s/%.*s", kv_dir(), (int)(slash - key), key);
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -EIO;
    }
    snprintf(out, cap, "%s/%s", kv_dir(), key);
    return 0;
}

int nn_osal_kv_save(const char *key, const void *data, size_t len)
{
    char p[512], tmp[520];
    if (key_path(key, p, sizeof p, true)) return -EINVAL;
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -EIO;
    size_t w = fwrite(data, 1, len, f);
    int rc = (fclose(f) == 0 && w == len) ? 0 : -EIO;
    if (rc == 0 && rename(tmp, p) != 0) rc = -EIO;   /* atomic replace */
    if (rc) unlink(tmp);
    return rc;
}

int nn_osal_kv_delete(const char *key)
{
    char p[512];
    if (key_path(key, p, sizeof p, false)) return -EINVAL;
    return (unlink(p) == 0 || errno == ENOENT) ? 0 : -EIO;
}

int nn_osal_kv_load_all(void)
{
    for (int i = 0; i < s_nreg; i++) {
        char dir[300];
        snprintf(dir, sizeof dir, "%s/%s", kv_dir(), s_reg[i].prefix);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.' || strstr(de->d_name, ".tmp")) continue;
            char p[560];
            snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
            FILE *f = fopen(p, "rb");
            if (!f) continue;
            uint8_t buf[512];
            size_t n = fread(buf, 1, sizeof buf, f);
            fclose(f);
            if (n > 0) s_reg[i].cb(de->d_name, buf, n, s_reg[i].user);
        }
        closedir(d);
    }
    return 0;
}

/* ── DFU (not applicable on hosted Linux) ────────────────────────────── */

struct nn_osal_dfu_ctx { int unused; };

nn_osal_dfu_ctx_t *nn_osal_dfu_ctx_alloc(void) { return calloc(1, sizeof(struct nn_osal_dfu_ctx)); }
void               nn_osal_dfu_ctx_free(nn_osal_dfu_ctx_t *ctx) { free(ctx); }
int nn_osal_dfu_begin(nn_osal_dfu_ctx_t *ctx, size_t expected_size)
{ (void)ctx; (void)expected_size; return -ENOSYS; }
int nn_osal_dfu_write(nn_osal_dfu_ctx_t *ctx, const uint8_t *data, size_t len)
{ (void)ctx; (void)data; (void)len; return -ENOSYS; }
int nn_osal_dfu_finalise(nn_osal_dfu_ctx_t *ctx) { (void)ctx; return -ENOSYS; }
int nn_osal_dfu_request_upgrade(bool permanent) { (void)permanent; return -ENOSYS; }
int nn_osal_dfu_confirm(void) { return 0; }   /* hosted images need no confirm */
