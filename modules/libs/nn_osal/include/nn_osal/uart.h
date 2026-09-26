/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * nn_osal/uart.h — interrupt-driven UART RX.
 *
 * The nn project uses UARTs for two things:
 *   1. The Spinel/HDLC link between an NCP host MCU and its radio NCP
 *      (high data rate, RX is interrupt-driven with FIFO reads).
 *   2. Debug console (handled by Zephyr's shell subsystem; not this API).
 *
 * The API mirrors Zephyr's `uart_irq_*` family because that's the only
 * useful pattern across embedded RTOSes — polled UART can't keep up
 * with 460 kbaud Spinel, and DMA-driven UART has wildly divergent APIs
 * across vendors.  IRQ + FIFO is the common denominator.
 *
 * On POSIX a future backend would wrap an O_NONBLOCK termios fd +
 * epoll; the callback would fire from an internal RX thread.
 */

typedef struct nn_osal_uart nn_osal_uart_t;

/* Callback fires on UART IRQ.  Handler typically loops on
 *   nn_osal_uart_irq_update(uart) → nn_osal_uart_irq_rx_ready(uart) →
 *   nn_osal_uart_fifo_read(uart, buf, sizeof buf) → process bytes.
 */
typedef void (*nn_osal_uart_irq_cb_t)(nn_osal_uart_t *uart, void *user);

/* Look up a board-defined UART by logical name (e.g. "ncp" for the
 * Spinel link).  Returns NULL if no registration with that name. */
nn_osal_uart_t *nn_osal_uart_get(const char *logical_name);

/* Boards call this from nn_board.c to publish a UART under a logical
 * name.  *uart storage is caller-owned (typically a file-static). */
int nn_osal_uart_register(const char *logical_name, nn_osal_uart_t *uart);

/* True if the underlying hardware is initialised + ready. */
bool nn_osal_uart_is_ready(const nn_osal_uart_t *uart);

/* ── IRQ-driven RX (mirrors Zephyr) ─────────────────────────────── */

void nn_osal_uart_irq_callback_set(nn_osal_uart_t *uart,
                                   nn_osal_uart_irq_cb_t cb,
                                   void *user);

void nn_osal_uart_irq_rx_enable(nn_osal_uart_t *uart);
void nn_osal_uart_irq_rx_disable(nn_osal_uart_t *uart);

/* Polling helpers callers use from inside the IRQ callback. */
bool nn_osal_uart_irq_is_pending(nn_osal_uart_t *uart);
int  nn_osal_uart_irq_update(nn_osal_uart_t *uart);
bool nn_osal_uart_irq_rx_ready(nn_osal_uart_t *uart);

/* Read up to *len* bytes; returns bytes read (>=0) or negative errno. */
int  nn_osal_uart_fifo_read(nn_osal_uart_t *uart, uint8_t *buf, size_t len);

/* Polled single-byte TX.  Blocks if the HW TX FIFO is full.  Used by
 * the Spinel TX path that holds a mutex across a full frame — IRQ-TX
 * would require dropping/re-acquiring the lock per byte, which is
 * worse than polled at the speeds we care about. */
void nn_osal_uart_poll_out(nn_osal_uart_t *uart, uint8_t byte);

#include "internal/backend_uart.h"
