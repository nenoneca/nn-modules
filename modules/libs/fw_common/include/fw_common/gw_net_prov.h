/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gw_net_prov — TCP/mDNS provisioning transport for the gateway daemon.
 *
 * Mirrors the BLE GATT provisioning surface (fw_common/gw_ble_prov.h)
 * one-to-one over an open TCP socket so a hub on the same LAN can
 * provision the gateway without BLE.  The protocol module
 * (gw_ble_prov.c) is reused unchanged; this header is the wire
 * adapter.  Same ECIES envelopes, same STATUS state machine, same
 * COMMIT semantics.
 *
 * Discovery
 *   gateway publishes  _nn-gw._tcp.local  via avahi
 *                       instance name = "nn-gw-XXXXXX" (last 3 B of MAC)
 *                       port          = 8770 (configurable)
 *   hub side scans the same service via python-zeroconf, or skips the
 *   scan if given an explicit `--addr <host>:<port>`.
 *
 * Wire framing (TCP stream of frames):
 *
 *   ┌────────┬────────────┬───────────────────────┐
 *   │ u8 type│ u16 len BE │ payload (len bytes)   │
 *   └────────┴────────────┴───────────────────────┘
 *
 *   Max payload: 4096 bytes — well above the ECIES envelope for the OT
 *   operational dataset (largest single write, ~430 B).
 *
 * Frame types (request, hub → gateway):
 *   0x01  REQ_GET_INFO        no payload
 *                              expect REP_INFO back
 *   0x02  REQ_HUB_X25519      32 B  (plaintext hub static x25519 pub)
 *   0x03  REQ_WIFI_CRED       ECIES envelope bytes
 *   0x04  REQ_HUB_HOST        ECIES envelope bytes
 *   0x05  REQ_HUB_IDENTITY    ECIES envelope bytes
 *   0x06  REQ_OT_DATASET      ECIES envelope bytes
 *   0x07  REQ_COMMIT          1 B   (0x00 clear, 0x01 apply)
 *
 * Reply types (gateway → hub):
 *   0x80  REP_INFO            106 B (the same blob a GATT GW_INFO read
 *                                    returns: schema=2 + gw_id +
 *                                    p256_pub + x25519_pub)
 *   0x81  REP_OK              no payload — write accepted
 *   0x82  REP_ERR             1 B   (abs(errno) clamped to 0..255)
 *   0x90  NOTIFY_STATUS       1 B   (status byte, server-initiated)
 *
 * Threading
 *   Backend handles one connection at a time.  Frames are processed
 *   sequentially on the connection thread; the protocol module
 *   serializes naturally because there's no separate BT-RX thread to
 *   race the decrypt worker.  PSA decrypt runs inline on the same
 *   thread (~ms on a Pi, no LL timeout to worry about).
 *
 * Reboot semantics
 *   On COMMIT success the backend calls _exit(0); the parent
 *   supervisor (systemd or `while true; do ...; done` wrapper) is
 *   expected to restart the daemon, which will then load the
 *   persisted gw_provision blob and proceed to its steady-state
 *   role.
 */

#ifndef FW_COMMON_GW_NET_PROV_H_
#define FW_COMMON_GW_NET_PROV_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_NET_PROV_DEFAULT_PORT 8770

/* Frame types — kept in sync with the Python client. */
enum {
	GW_NET_REQ_GET_INFO     = 0x01,
	GW_NET_REQ_HUB_X25519   = 0x02,
	GW_NET_REQ_WIFI_CRED    = 0x03,
	GW_NET_REQ_HUB_HOST     = 0x04,
	GW_NET_REQ_HUB_IDENTITY = 0x05,
	GW_NET_REQ_OT_DATASET   = 0x06,
	GW_NET_REQ_COMMIT       = 0x07,

	GW_NET_REP_INFO         = 0x80,
	GW_NET_REP_OK           = 0x81,
	GW_NET_REP_ERR          = 0x82,

	GW_NET_NOTIFY_STATUS    = 0x90,
};

/* Largest single TCP-frame payload accepted.  Sized for the OT dataset
 * ECIES envelope (~430 B base64+JSON) with comfortable headroom. */
#define GW_NET_PROV_MAX_PAYLOAD 4096

/* Start the listener.  `port = 0` selects GW_NET_PROV_DEFAULT_PORT.
 * Binds on 0.0.0.0 (all interfaces).  Optionally publishes
 * `_nn-gw._tcp.local` via avahi if `mdns` is non-zero.  Returns 0 on
 * success or a negative errno. */
int  gw_net_prov_start(uint16_t port, int mdns);

/* Run the accept loop until a SIGINT/SIGTERM or _exit() from the
 * commit path.  Returns 0 on clean shutdown, < 0 on error. */
int  gw_net_prov_run(void);

/* Tear down.  Safe to call from a signal handler. */
void gw_net_prov_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* FW_COMMON_GW_NET_PROV_H_ */
