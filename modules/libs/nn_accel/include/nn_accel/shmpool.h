/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Zero-copy frame pool shared with an accelerator service.
 *
 * The producer creates a pool (memfd, or an imported dma-buf), maps it
 * once, and passes the fd to the service ONCE with SCM_RIGHTS.  Every
 * request then names {pool, slot} — a frame is never written to a socket.
 *
 * Slot ownership is explicit and single-writer:
 *   FREE -> (producer acquires) OWNED -> (sent) BUSY -> (released) FREE
 * A BUSY slot is never handed out again, so the consumer cannot read a
 * frame that is being overwritten.  State lives in a header INSIDE the
 * shared mapping so both sides see it without a round trip. */

#define NN_POOL_MAGIC   0x4E4E5031u   /* "NNP1" */
#define NN_POOL_SLOTS_MAX 64

typedef enum { NN_SLOT_FREE = 0, NN_SLOT_OWNED = 1, NN_SLOT_BUSY = 2 } nn_slot_state_t;

typedef struct {                 /* first bytes of the mapping */
    uint32_t magic;
    uint32_t slots;
    uint32_t slot_bytes;
    uint32_t data_off;           /* byte offset of slot 0 */
    uint32_t width, height, stride, format;
    uint32_t state[NN_POOL_SLOTS_MAX];   /* nn_slot_state_t, atomic access */
    uint64_t seq[NN_POOL_SLOTS_MAX];
} nn_pool_hdr_t;

typedef struct {
    int            fd;           /* memfd/dma-buf — pass this with SCM_RIGHTS */
    void          *map;
    size_t         map_len;
    nn_pool_hdr_t *hdr;
    uint8_t       *data;
    bool           owner;        /* created it (vs mapped someone else's) */
} nn_pool_t;

/* Producer side: create + map. slot_bytes is rounded up to a page. */
int  nn_pool_create(nn_pool_t *p, uint32_t slots, uint32_t slot_bytes,
                    uint32_t w, uint32_t h, uint32_t stride, uint32_t format);
/* Consumer side: map a pool received over a socket. */
int  nn_pool_map(nn_pool_t *p, int fd);
void nn_pool_close(nn_pool_t *p);

/* Producer: take a FREE slot (returns -EAGAIN when all are in flight —
 * that is the backpressure signal, drop the frame). */
int   nn_pool_acquire(nn_pool_t *p, uint32_t *slot);
void *nn_pool_slot_ptr(nn_pool_t *p, uint32_t slot);
/* Producer: hand it to the consumer (OWNED -> BUSY). */
int   nn_pool_publish(nn_pool_t *p, uint32_t slot, uint64_t seq);
/* Consumer: finished reading (BUSY -> FREE). */
int   nn_pool_release(nn_pool_t *p, uint32_t slot);

/* fd passing over AF_UNIX (SCM_RIGHTS). */
int nn_send_fd(int sock, const void *msg, size_t len, int fd);
int nn_recv_fd(int sock, void *msg, size_t cap, int *fd_out);
