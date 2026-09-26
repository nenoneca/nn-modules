/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal buffer mapping — POSIX (dma-buf via mmap). */
#include "nn_osal/buf.h"
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

int nn_osal_buf_map(const nn_osal_buf_t *b, void **out)
{
    if (!b || !out) return -EINVAL;
    if (b->kind == NN_OSAL_BUF_MEM) {
        if (!b->ptr) return -EINVAL;
        *out = b->ptr;
        return 0;
    }
    long pg = sysconf(_SC_PAGESIZE);
    off_t base = b->offset & ~(off_t)(pg - 1);
    size_t skew = b->offset - (uint32_t)base;
    void *m = mmap(NULL, b->size + skew, PROT_READ, MAP_SHARED, b->fd, base);
    if (m == MAP_FAILED) return -errno;
    *out = (uint8_t *)m + skew;
    return 0;
}

void nn_osal_buf_unmap(const nn_osal_buf_t *b, void *mapped)
{
    if (!b || !mapped || b->kind == NN_OSAL_BUF_MEM) return;
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t p = (uintptr_t)mapped;
    uintptr_t base = p & ~(uintptr_t)(pg - 1);
    munmap((void *)base, b->size + (p - base));
}
