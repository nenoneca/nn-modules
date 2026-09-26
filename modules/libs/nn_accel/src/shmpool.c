/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <nn_accel/shmpool.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#include <sys/syscall.h>
static int memfd_create(const char *n, unsigned f) {
    return (int)syscall(__NR_memfd_create, n, f);
}
#define MFD_CLOEXEC 0x0001U
#endif

static size_t page_round(size_t n)
{
    long pg = sysconf(_SC_PAGESIZE);
    return (n + (size_t)pg - 1) & ~((size_t)pg - 1);
}

int nn_pool_create(nn_pool_t *p, uint32_t slots, uint32_t slot_bytes,
                   uint32_t w, uint32_t h, uint32_t stride, uint32_t format)
{
    if (!p || slots == 0 || slots > NN_POOL_SLOTS_MAX || slot_bytes == 0)
        return -EINVAL;
    memset(p, 0, sizeof *p);
    size_t sb = page_round(slot_bytes);
    size_t off = page_round(sizeof(nn_pool_hdr_t));
    size_t len = off + sb * slots;

    int fd = memfd_create("nn_pool", MFD_CLOEXEC);
    if (fd < 0) return -errno;
    if (ftruncate(fd, (off_t)len) != 0) { close(fd); return -errno; }
    void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -errno; }

    p->fd = fd; p->map = m; p->map_len = len; p->owner = true;
    p->hdr = (nn_pool_hdr_t *)m;
    p->data = (uint8_t *)m + off;
    memset(p->hdr, 0, sizeof *p->hdr);
    p->hdr->magic = NN_POOL_MAGIC;
    p->hdr->slots = slots;
    p->hdr->slot_bytes = (uint32_t)sb;
    p->hdr->data_off = (uint32_t)off;
    p->hdr->width = w; p->hdr->height = h;
    p->hdr->stride = stride; p->hdr->format = format;
    return 0;
}

int nn_pool_map(nn_pool_t *p, int fd)
{
    if (!p || fd < 0) return -EINVAL;
    memset(p, 0, sizeof *p);
    /* map the header first: its size is fixed and known before the rest */
    size_t hlen = page_round(sizeof(nn_pool_hdr_t));
    void *h = mmap(NULL, hlen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (h == MAP_FAILED) return -errno;
    nn_pool_hdr_t *hdr = h;
    if (hdr->magic != NN_POOL_MAGIC || hdr->slots == 0 ||
        hdr->slots > NN_POOL_SLOTS_MAX) {
        munmap(h, hlen);
        return -EPROTO;                 /* not ours / truncated */
    }
    size_t len = hdr->data_off + (size_t)hdr->slot_bytes * hdr->slots;
    munmap(h, hlen);
    void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return -errno;
    p->fd = fd; p->map = m; p->map_len = len; p->owner = false;
    p->hdr = m;
    p->data = (uint8_t *)m + p->hdr->data_off;
    return 0;
}

void nn_pool_close(nn_pool_t *p)
{
    if (!p) return;
    if (p->map) munmap(p->map, p->map_len);
    if (p->fd >= 0) close(p->fd);
    memset(p, 0, sizeof *p);
    p->fd = -1;
}

int nn_pool_acquire(nn_pool_t *p, uint32_t *slot)
{
    if (!p || !p->hdr || !slot) return -EINVAL;
    for (uint32_t i = 0; i < p->hdr->slots; i++) {
        uint32_t expect = NN_SLOT_FREE;
        if (__atomic_compare_exchange_n(&p->hdr->state[i], &expect,
                                        NN_SLOT_OWNED, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            *slot = i;
            return 0;
        }
    }
    return -EAGAIN;                    /* all in flight: caller drops */
}

void *nn_pool_slot_ptr(nn_pool_t *p, uint32_t slot)
{
    if (!p || !p->hdr || slot >= p->hdr->slots) return NULL;
    return p->data + (size_t)slot * p->hdr->slot_bytes;
}

int nn_pool_publish(nn_pool_t *p, uint32_t slot, uint64_t seq)
{
    if (!p || !p->hdr || slot >= p->hdr->slots) return -EINVAL;
    p->hdr->seq[slot] = seq;
    /* release: the consumer must see the pixels before the state flip */
    __atomic_store_n(&p->hdr->state[slot], NN_SLOT_BUSY, __ATOMIC_RELEASE);
    return 0;
}

int nn_pool_release(nn_pool_t *p, uint32_t slot)
{
    if (!p || !p->hdr || slot >= p->hdr->slots) return -EINVAL;
    __atomic_store_n(&p->hdr->state[slot], NN_SLOT_FREE, __ATOMIC_RELEASE);
    return 0;
}

int nn_send_fd(int sock, const void *msg, size_t len, int fd)
{
    struct iovec io = { .iov_base = (void *)msg, .iov_len = len };
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u = {0};
    struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1 };
    if (fd >= 0) {
        mh.msg_control = u.buf; mh.msg_controllen = sizeof u.buf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof fd);
    }
    ssize_t n = sendmsg(sock, &mh, 0);
    return n < 0 ? -errno : (int)n;
}

int nn_recv_fd(int sock, void *msg, size_t cap, int *fd_out)
{
    struct iovec io = { .iov_base = msg, .iov_len = cap };
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u = {0};
    struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1,
                         .msg_control = u.buf, .msg_controllen = sizeof u.buf };
    ssize_t n = recvmsg(sock, &mh, 0);
    if (n < 0) return -errno;
    if (fd_out) {
        *fd_out = -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                memcpy(fd_out, CMSG_DATA(c), sizeof(int));
    }
    return (int)n;
}
