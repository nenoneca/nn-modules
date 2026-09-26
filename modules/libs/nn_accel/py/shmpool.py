"""Python side of the nn_accel zero-copy frame pool (see shmpool.h).

Layout must match the C header EXACTLY — a C camera creates the pool and a
Python service maps it, so a mismatch here is silent corruption rather than
an error.  Kept as one struct format string so the two definitions can be
compared by eye.

    uint32 magic, slots, slot_bytes, data_off, width, height, stride, format
    uint32 state[64]      # FREE / OWNED / BUSY
    uint64 seq[64]
"""
from __future__ import annotations

import mmap
import struct

MAGIC = 0x4E4E5031          # "NNP1"
SLOTS_MAX = 64
FREE, OWNED, BUSY = 0, 1, 2

_HDR = "<8I"                       # 32 B
_STATE_OFF = 32
_SEQ_OFF = _STATE_OFF + 4 * SLOTS_MAX      # 288
_HDR_BYTES = _SEQ_OFF + 8 * SLOTS_MAX      # 800


class Pool:
    """Maps a pool fd received over AF_UNIX (SCM_RIGHTS). Never copies."""

    def __init__(self, fd: int):
        self.fd = fd
        # map the header first: its size is fixed, the total is not
        head = mmap.mmap(fd, _HDR_BYTES, mmap.MAP_SHARED,
                         mmap.PROT_READ | mmap.PROT_WRITE)
        (magic, self.slots, self.slot_bytes, self.data_off,
         self.width, self.height, self.stride, self.format) = \
            struct.unpack_from(_HDR, head, 0)
        head.close()
        if magic != MAGIC or not (0 < self.slots <= SLOTS_MAX):
            raise ValueError(f"not an nn_accel pool (magic={magic:#x})")
        self.size = self.data_off + self.slot_bytes * self.slots
        self.map = mmap.mmap(fd, self.size, mmap.MAP_SHARED,
                             mmap.PROT_READ | mmap.PROT_WRITE)
        self.view = memoryview(self.map)

    def frame(self, slot: int) -> memoryview:
        """Zero-copy view of a slot's pixels — no bytes() here, ever.

        The view points INTO the shared mapping, so it must not outlive the
        release of that slot (the producer may refill it) nor the pool
        itself.  Release it, or use `with pool.frame(s) as f:`."""
        if not 0 <= slot < self.slots:
            raise IndexError(slot)
        off = self.data_off + slot * self.slot_bytes
        return self.view[off:off + self.slot_bytes]

    def state(self, slot: int) -> int:
        return struct.unpack_from("<I", self.map, _STATE_OFF + 4 * slot)[0]

    def seq(self, slot: int) -> int:
        return struct.unpack_from("<Q", self.map, _SEQ_OFF + 8 * slot)[0]

    def release(self, slot: int) -> None:
        """Hand the slot back to the producer (BUSY -> FREE)."""
        struct.pack_into("<I", self.map, _STATE_OFF + 4 * slot, FREE)

    def close(self) -> None:
        """Unmap and close.  Never raises: shutdown must not be the thing
        that fails, and an outstanding frame() view (mmap refuses to close
        while one exists) should not leak the fd."""
        import os
        try:
            self.view.release()
        except (BufferError, ValueError):
            pass                    # caller still holds a frame view
        try:
            self.map.close()
        except (BufferError, ValueError):
            pass                    # GC unmaps once the last view is dropped
        try:
            os.close(self.fd)
        except OSError:
            pass
