# P4↔C6 link — bandwidth upgrade paths

Status as of 2026-06-20: link runs **dual (2-bit) SPI @ 8 MHz**, rock-solid
(P4 dropped=0, C6 5/165727 ≈ 0.003%, full 1080p H.264 → WebRTC). Raw ceiling
~16 Mbit/s = ~2× headroom over the ~4 Mbps video. **No upgrade is needed today.**
This note records the two ways to get more bandwidth if a future
higher-bitrate / higher-resolution / higher-fps mode ever makes dual the
bottleneck, so the analysis isn't re-derived from scratch.

## Why not just use quad (4-bit) SPI?

Quad is a **dead end on the current transport** — and the reason is the
*slave receive* engine, not wiring, resistors, signal integrity, or clock.

Traced through ESP-IDF v6.0.1 (`essl_spi.c`, `spi_slave_hd.c`,
`esp32c6/spi_ll.h`, `spi_common.c`):

- **Master side is correct & symmetric.** `essl_spi_wrdma(QIO)` emits opcode
  `0x03|0x20 = 0x23` (CMD 1-line, ADDR 1-line, DATA 4-line) + 8 dummy bits —
  symmetric with the working dual `0x13` and single `0x03`. The P4 master
  quad-write is the standard, proven QSPI-flash write path.
- **The slave never programs a data-line width.** The only function that sets
  `ctrl.fread_dual/quad` / `user.fwrite_dual/quad` is
  `spi_ll_master_set_line_mode()` — **master-only**. `spi_ll_slave_hd_init()`
  zeroes ctrl/user and sets `sio=0`; the slave-HD driver/HAL never touch those
  bits. So the C6 slave relies entirely on the GPSPI-ver2 **hardware command
  auto-decode** (opcode high nibble: `0x10`=dual, `0x20`=quad) to pick the
  per-segment data width. That auto-decode brings up single + **dual** RX but
  **not quad RX** in segment-DMA mode → quad WRDMA receives nothing.
- **Everything else is ruled out.** GPIO routing connects all four input lines
  (D0 `spid_in` always; D1 `spiq_in` via the DUAL pin-capability flag in
  `spi_common.c`; D2/D3 `spiwp_in`/`spihd_in` when quadwp/hd are set). The
  official `spi_slave_hd/segment_mode` example uses only single-line; the
  component ships no quad-RX test. Failure was total silence, clock-independent
  (identical at 2 MHz and 500 kHz) — not corruption, so not SI/resistors.
- **Note:** the register/handshake path is hardcoded single-line
  (`essl_spi_read_reg` → `essl_spi_rdbuf(..., 0)`), so "the link connects in
  quad" only ever proved single-line; the sole quad traffic was bulk video
  WRDMA = exactly what failed.

Conclusion: `spi_slave_hd` segment mode has a working **single + dual** receive
path and a broken/unsupported **quad receive** path. Dual is the multibit
ceiling for the link **as currently wired** (P4 master writes → C6 slave
receives).

## Upgrade path A — RDDMA-direction flip (4-bit over SPI)

The quad direction that *does* work on the slave is **TX** (slave drives 4
lines out, master samples) — i.e. **RDDMA** (master reads from slave). Driving
4 outputs is easier in silicon than sampling 4 aligned inputs; the old bench
note already saw slave→master quad reads succeed.

Today video is on the broken direction:

```
TODAY:   P4(master) --WRDMA quad--> C6(slave RX)    ← slave 4-line RX = BROKEN
FLIP:    C6(master) <--RDDMA quad-- P4(slave TX)     ← slave 4-line TX = WORKS
```

So **swap the SPI roles**: make the **C6 the SPI master** and the **P4 the SPI
slave**. The C6 master issues RDDMA to pull video out of the P4 slave's TX DMA;
the P4 (video source) sits on the working quad-TX engine, the C6 quad-reads
(the standard QSPI-flash read path).

Cost (this is a rearchitecture, not a Kconfig flag):

1. **Backend swap in `nn_link`** — P4 runs `spi_slave_hd`, C6 runs `essl_spi`
   master. The two backends trade chips.
2. **Flow control inverts** — today the C6 slave publishes "data ready" (HS GPIO
   + shared regs) and the P4 master drains. After the flip the *P4 slave*
   publishes video-ready and the *C6 master* polls/pulls; the whole credit
   scheme moves sides.
3. **Control channel C6→P4** becomes master-WRDMA (single-line, fine) but is
   re-plumbed.
4. **Boot/reset interplay** — the C6 already owns the P4's boot/reset straps
   (`nn_p4ctl`), so C6-as-link-master is arguably *more* natural, but the P4
   must bring up its SPI slave + pre-queue TX buffers before the C6 starts
   reading (startup reorders).

Gain: roughly 2× the raw ceiling vs dual (4-bit vs 2-bit at the same clock).

## Upgrade path B — SDIO 4-bit (preferred for real bandwidth)

The original transport. SDIO has a purpose-built 4-bit slave receive engine
(no `spi_slave_hd` quad-RX limitation) and a higher clock ceiling
(4-bit × up to ~40 MHz ≈ 160 Mbit/s vs dual SPI's 16 Mbit/s). It enumerated and
the control plane worked on the bench; the blocker was bulk-data corruption from
**missing external pull-ups** — SDIO mandates ~10 kΩ to 3V3 on CMD + D0..D3,
which needs a real PCB (open-drain bus, unlike push-pull SPI that works bare on
jumpers).

Gain: the largest ceiling, and it's the *clean* high-bandwidth path — no SPI
role rearchitecture. Cost: a PCB with the pull-ups (and equal-length short
traces, common ground).

## Recommendation

Stay on **dual @ 8 MHz** until a concrete higher-bandwidth video mode needs
more. When that day comes, prefer **SDIO 4-bit on a PCB (path B)** over the
SPI role-flip (path A): SDIO gives more headroom and avoids rearchitecting the
master/slave roles and flow control. Keep path A in reserve as the
"4-bit without a board respin" option.
