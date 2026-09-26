/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Opaque-ish frame/buffer handle passed between capture and consumers
 * (nn_infer, encoders).  Apps treat it as a black box; backends map it.
 *  - MEM:    plain memory (every platform)
 *  - DMABUF: Linux dma-buf fd (zero-copy between V4L2/GStreamer/accelerators)
 */
typedef enum {
    NN_OSAL_BUF_MEM    = 0,
    NN_OSAL_BUF_DMABUF = 1,
} nn_osal_buf_kind_t;

typedef struct {
    nn_osal_buf_kind_t kind;
    void    *ptr;      /* MEM: payload (NULL for DMABUF until mapped) */
    int      fd;       /* DMABUF: the dma-buf fd (-1 for MEM) */
    size_t   size;     /* payload bytes */
    uint32_t offset;   /* payload offset inside the fd mapping */
} nn_osal_buf_t;

static inline nn_osal_buf_t nn_osal_buf_mem(void *p, size_t n)
{ nn_osal_buf_t b = { NN_OSAL_BUF_MEM, p, -1, n, 0 }; return b; }

static inline nn_osal_buf_t nn_osal_buf_dmabuf(int fd, size_t n, uint32_t off)
{ nn_osal_buf_t b = { NN_OSAL_BUF_DMABUF, 0, fd, n, off }; return b; }

/* Map for CPU access.  MEM: returns ptr.  DMABUF: mmap (POSIX backend).
 * Returns 0 and sets *out, or -errno. Unmap with the same buf + pointer. */
int  nn_osal_buf_map(const nn_osal_buf_t *b, void **out);
void nn_osal_buf_unmap(const nn_osal_buf_t *b, void *mapped);
