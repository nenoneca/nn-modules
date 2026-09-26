/* SPDX-License-Identifier: Apache-2.0 */
/* nn_infer PAL — talk to nn-inferd instead of owning an accelerator.
 *
 * Same nn_infer API as the in-process PALs, so an app switches by build
 * flag alone.  Frames go through an nn_accel shm pool (fd passed once with
 * SCM_RIGHTS); each run sends ~25 bytes and blocks for the reply, which is
 * what the caller's own inference thread already expects. */
#include <nn_infer/nn_infer.h>
#include <nn_accel/shmpool.h>
#include <nn_osal/log.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

NN_OSAL_LOG_MODULE(nn_infer);

/* Hand-rolled protobuf writing/reading: three field types, no runtime.
 * Pulling libprotobuf-c onto an MCU-adjacent build for 25-byte messages
 * would cost more than it saves. */
static size_t pb_varint(uint8_t *b, uint64_t v)
{
    size_t n = 0;
    do { b[n] = (v & 0x7F) | (v > 0x7F ? 0x80 : 0); v >>= 7; n++; } while (v);
    return n;
}
static size_t pb_tag(uint8_t *b, uint32_t field, uint32_t wire)
{ return pb_varint(b, ((uint64_t)field << 3) | wire); }
static size_t pb_u32(uint8_t *b, uint32_t field, uint64_t val)
{
    if (!val) return 0;                     /* proto3: skip defaults */
    size_t n = pb_tag(b, field, 0);
    return n + pb_varint(b + n, val);
}

#define SOCK_ENV   "NN_INFERD_SOCK"
#define SOCK_DFLT  "/run/nn-inferd.sock"
#define POOL_SLOTS 4

static int        s_fd = -1;
static nn_pool_t  s_pool;
static uint32_t   s_pool_id;
static uint64_t   s_seq;
static uint32_t   s_last_try_ms;
static nn_infer_caps_t s_caps = {
    .width = 640, .height = 640, .format = NN_INFER_FMT_RGB888,
    .fps = 5, .nclasses = 80, .model = "nn-inferd",
};

static int dial(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return -errno;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        int e = -errno; close(fd); return e;
    }
    return fd;
}

static int connect_and_handshake(void)
{
    const char *path = getenv(SOCK_ENV);
    s_fd = dial(path && *path ? path : SOCK_DFLT);
    if (s_fd < 0) return s_fd;
    /* Msg{ id=1, hello=HelloReq{ client } } */
    uint8_t m[96]; size_t n = 0;
    n += pb_u32(m + n, 1, 1);
    const char *who = getenv("NN_CAM_ID"); if (!who) who = "camera";
    size_t wl = strlen(who);
    n += pb_tag(m + n, 2, 2); n += pb_varint(m + n, wl + 2);
    n += pb_tag(m + n, 1, 2); n += pb_varint(m + n, wl);
    memcpy(m + n, who, wl); n += wl;
    if (send(s_fd, m, n, 0) < 0) { close(s_fd); s_fd = -1; return -errno; }
    uint8_t r[512];
    if (recv(s_fd, r, sizeof r, 0) <= 0) { close(s_fd); s_fd = -1; return -EIO; }

    size_t slot_bytes = (size_t)s_caps.width * s_caps.height * 3;
    int rc = nn_pool_create(&s_pool, POOL_SLOTS, (uint32_t)slot_bytes,
                            s_caps.width, s_caps.height, s_caps.width * 3, 1);
    if (rc) { close(s_fd); s_fd = -1; return rc; }

    /* Msg{ id=2, open_pool=PoolSpec{ owner, slots, slot_bytes, fmt, w,h,stride } } */
    uint8_t sp[128]; size_t k = 0;
    k += pb_tag(sp + k, 1, 2); k += pb_varint(sp + k, wl);   /* owner */
    memcpy(sp + k, who, wl); k += wl;
    k += pb_u32(sp + k, 2, POOL_SLOTS);
    k += pb_u32(sp + k, 3, s_pool.hdr->slot_bytes);
    k += pb_u32(sp + k, 4, 1);                      /* PIX_RGB888 */
    k += pb_u32(sp + k, 5, s_caps.width);
    k += pb_u32(sp + k, 6, s_caps.height);
    k += pb_u32(sp + k, 7, s_caps.width * 3);

    uint8_t om[192]; size_t o = 0;
    o += pb_u32(om + o, 1, 2);                      /* id */
    o += pb_tag(om + o, 4, 2); o += pb_varint(om + o, k);
    memcpy(om + o, sp, k); o += k;
    if (nn_send_fd(s_fd, om, o, s_pool.fd) < 0) return -EIO;
    int rn = nn_recv_fd(s_fd, r, sizeof r, NULL);
    if (rn <= 0) return -EIO;
    /* PoolId is the only u32 in the reply; scan for field 5 (pool_id). */
    s_pool_id = 1;
    for (int i = 0; i + 2 < rn; i++)
        if (r[i] == ((5 << 3) | 2) && r[i + 2] == ((1 << 3) | 0))
            { s_pool_id = r[i + 3]; break; }
    NN_LOG_INF("nn-inferd ready (pool %u, %d slots)", s_pool_id, POOL_SLOTS);
    return 0;
}

int nn_infer_init(const char *model_dir)
{
    (void)model_dir;                        /* the daemon owns the model */
    int rc = connect_and_handshake();
    if (rc) {
        const char *path = getenv(SOCK_ENV);
        NN_LOG_ERR("nn-inferd unreachable at %s: %d",
                   path && *path ? path : SOCK_DFLT, rc);
    }
    return rc;
}

/* A daemon restart drops the connection AND the pool fd it was given.  The
 * camera must not lose edge inference until someone restarts it, so a broken
 * link is torn down and rebuilt from scratch — rate-limited, because a daemon
 * that is down stays down for a while and every retry costs a syscall storm. */
static void link_down(void)
{
    if (s_fd >= 0) { close(s_fd); s_fd = -1; }
    nn_pool_close(&s_pool);
}

static int link_retry(uint32_t now_ms)
{
    if (now_ms - s_last_try_ms < 5000u) return -ENOTCONN;
    s_last_try_ms = now_ms;
    if (connect_and_handshake() != 0) { link_down(); return -ENOTCONN; }
    NN_LOG_INF("nn-inferd reconnected");
    return 0;
}

void nn_infer_deinit(void)
{
    if (s_fd >= 0) { close(s_fd); s_fd = -1; }
    nn_pool_close(&s_pool);
}

int nn_infer_query_caps(nn_infer_caps_t *caps)
{
    if (!caps) return -EINVAL;
    *caps = s_caps;
    return 0;
}

int nn_infer_run(const nn_infer_input_t *in, nn_infer_result_t *out)
{
    if (!in || !out) return -EINVAL;
    if (s_fd < 0 && link_retry(in->ts_ms) != 0) return -ENOTCONN;
    uint32_t slot;
    if (nn_pool_acquire(&s_pool, &slot) != 0) return -EAGAIN;  /* backpressure */

    void *dst = nn_pool_slot_ptr(&s_pool, slot);
    void *src = NULL;
    int rc = nn_osal_buf_map(&in->buf, &src);
    if (rc) { nn_pool_release(&s_pool, slot); return rc; }
    memcpy(dst, src, (size_t)s_caps.width * s_caps.height * 3);
    nn_osal_buf_unmap(&in->buf, src);
    nn_pool_publish(&s_pool, slot, ++s_seq);

    uint8_t ir[64]; size_t k = 0;
    k += pb_u32(ir + k, 1, s_pool_id);
    k += pb_u32(ir + k, 2, slot);
    k += pb_u32(ir + k, 3, s_seq);
    k += pb_u32(ir + k, 4, in->ts_ms);
    k += pb_u32(ir + k, 6, 1000);                   /* deadline_ms */
    uint8_t m[96]; size_t n = 0;
    n += pb_u32(m + n, 1, (uint32_t)(s_seq & 0x7FFFFFFF));
    n += pb_tag(m + n, 6, 2); n += pb_varint(m + n, k);
    memcpy(m + n, ir, k); n += k;
    if (send(s_fd, m, n, 0) < 0) {
        nn_pool_release(&s_pool, slot);
        link_down(); s_last_try_ms = in->ts_ms;
        return -EIO;
    }

    uint8_t r[4096];
    int rn = (int)recv(s_fd, r, sizeof r, 0);
    if (rn <= 0) {                          /* 0 = daemon closed the socket */
        nn_pool_release(&s_pool, slot);
        link_down(); s_last_try_ms = in->ts_ms;
        return -EIO;
    }

    /* Parse InferResp: detections are field 3 (nested), each with 6 varints. */
    out->ts_ms = in->ts_ms;
    out->count = 0;
    for (int i = 0; i < rn && out->count < NN_INFER_MAX_DET; i++) {
        if (r[i] != ((3 << 3) | 2)) continue;       /* dets, length-delimited */
        int len = r[i + 1], p = i + 2, end = p + len;
        if (end > rn) break;
        uint32_t f[7] = {0};
        while (p < end) {
            uint32_t fld = r[p] >> 3; p++;
            uint64_t v = 0; int sh = 0;
            while (p < end && (r[p] & 0x80)) { v |= (uint64_t)(r[p] & 0x7F) << sh; sh += 7; p++; }
            if (p < end) { v |= (uint64_t)r[p] << sh; p++; }
            if (fld < 7) f[fld] = (uint32_t)v;
        }
        nn_infer_det_t *d = &out->det[out->count++];
        d->x = (uint16_t)f[1]; d->y = (uint16_t)f[2];
        d->w = (uint16_t)f[3]; d->h = (uint16_t)f[4];
        d->class_id = (uint16_t)f[5]; d->conf_x1000 = (uint16_t)f[6];
        i = end - 1;
    }
    /* the daemon releases the slot; do it locally too in case it did not */
    nn_pool_release(&s_pool, slot);
    return 0;
}
