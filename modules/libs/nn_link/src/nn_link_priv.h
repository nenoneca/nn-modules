/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "sdkconfig.h"          /* NN_BOARD_ pin defaults (NN_LINK_SPI / SDIO) */
#include "nn_link/nn_link.h"

/* Handshake exchanged once the bus is up so BOTH ends agree the link is live
 * (the master sends HELLO, the slave answers ACK). */
#define NN_LINK_HELLO  "NNLINK-HELLO"
#define NN_LINK_ACK    "NNLINK-ACK"

/* ── SPI transport pin map (reuses the SDIO harness 1:1) ─────────────────
 *   signal     C6 (slave)   P4 (master)   old SDIO line
 *   SCLK       GPIO19       GPIO2         CLK
 *   MOSI(M->S) GPIO18       GPIO3         CMD
 *   MISO(S->M) GPIO20       GPIO15        D0
 *   CS         GPIO23       GPIO18        D3
 *   HANDSHAKE  GPIO21       GPIO16        D1   (slave->master data-ready)
 */
/* Pins come from the board profile (nn_registry Kconfig, NN_BOARD_*); the
 * defaults reproduce the combo bench harness shown above. */
#define NN_LINK_SPI_C6_SCLK   CONFIG_NN_LINK_SPI_C6_SCLK
#define NN_LINK_SPI_C6_MOSI   CONFIG_NN_LINK_SPI_C6_MOSI
#define NN_LINK_SPI_C6_MISO   CONFIG_NN_LINK_SPI_C6_MISO
#define NN_LINK_SPI_C6_CS     CONFIG_NN_LINK_SPI_C6_CS
#define NN_LINK_SPI_C6_HS     CONFIG_NN_LINK_SPI_C6_HS   /* output on C6 (single-line mode only) */

#define NN_LINK_SPI_P4_SCLK   CONFIG_NN_LINK_SPI_P4_SCLK
#define NN_LINK_SPI_P4_MOSI   CONFIG_NN_LINK_SPI_P4_MOSI
#define NN_LINK_SPI_P4_MISO   CONFIG_NN_LINK_SPI_P4_MISO
#define NN_LINK_SPI_P4_CS     CONFIG_NN_LINK_SPI_P4_CS
#define NN_LINK_SPI_P4_HS     CONFIG_NN_LINK_SPI_P4_HS   /* input on P4 (single-line mode only) */

/* Quad mode adds two data lines (IO2=WP, IO3=HD).  IO0=MOSI, IO1=MISO stay
 * as above; the D1(HS) and D2(spare) wires become IO2/IO3 — no re-wiring, so
 * WP shares the HANDSHAKE pin and HD is its own GPIO. */
#define NN_LINK_SPI_C6_WP     CONFIG_NN_LINK_SPI_C6_HS   /* IO2 — same wire as HS */
#define NN_LINK_SPI_C6_HD     CONFIG_NN_LINK_SPI_C6_HD   /* IO3 — the old spare D2 wire */
#define NN_LINK_SPI_P4_WP     CONFIG_NN_LINK_SPI_P4_HS   /* IO2 */
#define NN_LINK_SPI_P4_HD     CONFIG_NN_LINK_SPI_P4_HD   /* IO3 */

/* ── Per-wire diagnostic (single-line) ───────────────────────────────────
 * Override the master->slave data line (MOSI) onto a chosen physical quad
 * data wire, so single-line traffic exercises one wire at a time.  The wire
 * that fails to carry the link/video is the bad/unstable one. */
#if defined(CONFIG_NN_LINK_DIAG_MOSI) && (CONFIG_NN_LINK_DIAG_MOSI == 1)
#  undef  NN_LINK_SPI_C6_MOSI
#  undef  NN_LINK_SPI_P4_MOSI
#  define NN_LINK_SPI_C6_MOSI  CONFIG_NN_LINK_SPI_C6_MISO   /* IO1 wire */
#  define NN_LINK_SPI_P4_MOSI  CONFIG_NN_LINK_SPI_P4_MISO
#elif defined(CONFIG_NN_LINK_DIAG_MOSI) && (CONFIG_NN_LINK_DIAG_MOSI == 2)
#  undef  NN_LINK_SPI_C6_MOSI
#  undef  NN_LINK_SPI_P4_MOSI
#  define NN_LINK_SPI_C6_MOSI  CONFIG_NN_LINK_SPI_C6_HS   /* IO2 wire (free: HANDSHAKE must be off) */
#  define NN_LINK_SPI_P4_MOSI  CONFIG_NN_LINK_SPI_P4_HS
#elif defined(CONFIG_NN_LINK_DIAG_MOSI) && (CONFIG_NN_LINK_DIAG_MOSI == 3)
#  undef  NN_LINK_SPI_C6_MOSI
#  undef  NN_LINK_SPI_P4_MOSI
#  define NN_LINK_SPI_C6_MOSI  CONFIG_NN_LINK_SPI_C6_HD   /* IO3 wire */
#  define NN_LINK_SPI_P4_MOSI  CONFIG_NN_LINK_SPI_P4_HD
#endif

/* ── Framing ─────────────────────────────────────────────────────────────
 * Every packet goes on the wire as [u16 payload-len LE][payload][zero-pad],
 * padded so the raw length is a multiple of 4.  This (a) keeps transfers
 * 4-byte aligned and (b) restores true packet boundaries regardless of how a
 * transport coalesces or splits the byte stream.
 *
 * TAIL_GUARD: quad-line SPI master->slave writes consistently drop the last
 * couple of bytes (a clocking/CS-deassert artifact).  By reserving a few
 * always-padding bytes at the tail, those dropped bytes are never payload —
 * the receiver trusts the length prefix and recovers the full payload. */
#define NN_LINK_HDR_LEN     2u
#define NN_LINK_TAIL_GUARD  4u
#define NN_LINK_FRAME_LEN(payload)  \
    (((NN_LINK_HDR_LEN) + (payload) + (NN_LINK_TAIL_GUARD) + 3u) & ~3u)
/* Largest framed buffer a backend must be able to send/receive. */
#define NN_LINK_RAW_MAX   NN_LINK_FRAME_LEN(NN_LINK_MAX_PACKET)

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented in nn_link_common.c. */
void nn_link__deliver_rx(const uint8_t *data, size_t len);  /* invoke rx cb */
void nn_link__set_connected(bool up);                       /* flip + signal */
bool nn_link__ack_seen(void);                               /* master: HELLO ACKed */

/* Common framing layer <-> role backends:
 *   common  builds a frame and calls nn_link__raw_send() (backend transmit)
 *   backend hands each received raw chunk to nn_link__on_raw_rx() (deframe) */
esp_err_t nn_link__raw_send(const uint8_t *frame, size_t len);  /* in backend */
void      nn_link__on_raw_rx(const uint8_t *raw, size_t len);   /* in common  */

#ifdef __cplusplus
}
#endif
