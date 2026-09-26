/* SPDX-License-Identifier: Apache-2.0 */
/*
 * NCP link layer (platform-neutral header).
 *
 * Owns the UART connection to the NCP coprocessor chip, pumps RX bytes
 * through HDLC + Spinel decode, tracks outstanding requests by TID,
 * routes responses back to callers.
 *
 * The Zephyr version of this module lives at
 *   apps/ncp_host_esp32c6/src/ncp_link.{h,c}
 * and uses Zephyr's k_timeout_t / k_thread / k_sem / ring_buf APIs.
 * The Linux port at fw_common/src/platform/linux/ncp_link.c exposes
 * the SAME shape but with platform-portable timeouts and bring-up.
 */

#ifndef FW_COMMON_NCP_LINK_H_
#define FW_COMMON_NCP_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Linux-side bring-up ──────────────────────────────────────────── */

/*
 * Initialise the link for Linux.  Opens `uart_dev` 8N1, launches the RX
 * thread, allocates TID slots.  Returns 0 on success or a negative errno.
 *
 * Line settings follow the gateway host profile via environment:
 *   NCP_UART_FLOW = rtscts (default) | none
 *   NCP_UART_BAUD = 460800 (default) | 230400 | 115200
 * (BeagleY-AI: FLOW=none — its HAT header has no RTS/CTS; pair with an
 * NCP built with NN_GW_HOST=beagley-ai.)
 *
 * The Zephyr build provides its own bring-up via Devicetree
 * (ncp_link_init(void)) — Linux apps call this entrypoint instead.
 */
int ncp_link_init_linux(const char *uart_dev);

/* Open + configure an NCP UART fd with the same env-driven line settings
 * (for probing/discovery before committing to ncp_link_init_linux). */
int ncp_link_open_uart_linux(const char *path);

/* ── reset signal ─────────────────────────────────────────────────── */

void ncp_link_arm_reset_signal(void);
/* Returns 0 if the NCP rebooted within timeout_ms, -ETIMEDOUT otherwise. */
int  ncp_link_wait_reset(uint32_t timeout_ms);

/* ── OTA quiet-mode (Zephyr-side only currently — no-op on Linux) ── */
void ncp_link_set_quiet_for_ota(bool quiet);
bool ncp_link_is_quiet_for_ota(void);

/* ── Spinel request/reply ─────────────────────────────────────────── */

int ncp_link_get(uint32_t prop,
		 uint8_t *out_buf, size_t *out_len,
		 uint32_t timeout_ms);

int ncp_link_set_raw(uint32_t prop,
		     const uint8_t *payload, size_t payload_len,
		     uint32_t *out_last_status,
		     uint32_t timeout_ms);

int ncp_link_insert_raw(uint32_t prop,
			const uint8_t *payload, size_t payload_len,
			uint32_t *out_last_status,
			uint32_t timeout_ms);

int ncp_link_remove_raw(uint32_t prop,
			const uint8_t *payload, size_t payload_len,
			uint32_t *out_last_status,
			uint32_t timeout_ms);

int ncp_link_set_bool(uint32_t prop, bool v, uint32_t timeout_ms);
int ncp_link_set_u8  (uint32_t prop, uint8_t v, uint32_t timeout_ms);
int ncp_link_set_u16 (uint32_t prop, uint16_t v, uint32_t timeout_ms);

/* ── UDP forward (Border Agent commissioner relay) ───────────────── */

int ncp_link_udp_forward_tx(const uint8_t *payload, size_t payload_len,
			    uint16_t remote_port,
			    const uint8_t remote_ip6[16],
			    uint16_t local_port);

typedef void (*ncp_link_udp_fwd_cb_t)(const uint8_t *payload, size_t len,
				      uint16_t remote_port,
				      const uint8_t remote_ip6[16],
				      uint16_t local_port);

void ncp_link_set_udp_fwd_cb(ncp_link_udp_fwd_cb_t cb);

/* ── inbound STREAM_NET hook (provided by ncp_netif / ncp_tun) ───── */

/* The Linux ncp_link will call this when an unsolicited STREAM_NET
 * frame arrives — caller registers their TUN-writing handler. */
typedef void (*ncp_link_stream_net_cb_t)(const uint8_t *pkt, size_t len);
void ncp_link_set_stream_net_cb(ncp_link_stream_net_cb_t cb);

/* Any other unsolicited PROP_VALUE_IS (e.g. energy-scan results, scan
 * state, MGMT_SET status) is handed to this watcher, from the RX thread. */
typedef void (*ncp_link_prop_watch_cb_t)(uint32_t prop, const uint8_t *value, size_t len);
void ncp_link_set_prop_watch_cb(ncp_link_prop_watch_cb_t cb);

/* ── stats ────────────────────────────────────────────────────────── */

struct ncp_link_stats {
	uint32_t rx_frames;
	uint32_t rx_bytes;
	uint32_t tx_frames;
	uint32_t tx_bytes;
	uint32_t rx_bad_fcs;
	uint32_t rx_unsolicited;
};

void ncp_link_stats_get(struct ncp_link_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* FW_COMMON_NCP_LINK_H_ */
