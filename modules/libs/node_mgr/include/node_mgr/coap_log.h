/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/**
 * coap_log — Zephyr log backend that forwards logs via CoAP POST /log.
 *
 * Uses Zephyr's log backend API (like log_backend_mqtt) so ALL LOG_INF/
 * LOG_WRN/LOG_ERR messages are forwarded automatically.
 *
 * Call coap_log_start() once the hub address is known and the network
 * is up.  It creates a connected UDP socket and enables the backend.
 * Call coap_log_stop() to disable.
 *
 * Also provides coap_log_send(fmt, ...) for explicit log forwarding.
 *
 * Requires CONFIG_LOG_ALWAYS_RUNTIME=y in prj.conf (otherwise
 * compile-time filters for non-autostarted backends block all messages).
 */

/** Create connected socket to hub and enable the log backend. */
int coap_log_start(const char *hub_ipv6_addr);

/** Disable the log backend. */
void coap_log_stop(void);

/** Send a formatted log line to the hub (synchronous, best-effort). */
void coap_log_send(const char *fmt, ...);

/** Flush pending output (call from panic handler). */
void coap_log_flush(void);

/** Suspend / resume the deferred-logging thread.  Used by ota_client to
 *  halt logging around flash erase/write operations — on ESP32-C6 the
 *  flash XIP path corrupts the logging thread's stack if both run
 *  concurrently (see feedback_esp32c6_flash_xip.md).  No-op when the
 *  logging thread hasn't been registered yet. */
void coap_log_thread_suspend(void);
void coap_log_thread_resume(void);
