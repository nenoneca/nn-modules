/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gw_provision — gateway runtime configuration persisted across reboots.
 * Used by both the Zephyr ESP32-C6 gateway-host app and the Linux gw_linux
 * daemon (RPi).  Persistence is via fw_common/kvstore (settings on Zephyr,
 * files under $XDG_STATE_HOME/nn-gw/kv/ on Linux).
 *
 * Holds:
 *   - WiFi STA credentials (SSID + PSK)
 *   - Hub mDNS hostname (e.g. "nn-hub.local")
 *   - Hub identity:
 *       hub_id (8 bytes, = SHA256(hub_pubkey)[:8])
 *       hub_p256_pub (65 bytes, uncompressed)
 *   - Active OT operational dataset (Thread TLVs, ≤254 B)
 *
 * KV prefix: "gw_provision".
 */

#ifndef FW_COMMON_GW_PROVISION_H_
#define FW_COMMON_GW_PROVISION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_PROVISION_SSID_MAX     32
#define GW_PROVISION_PSK_MAX      64
#define GW_PROVISION_HUB_MDNS_MAX 64
#define GW_PROVISION_HUB_ID_LEN   8
#define GW_PROVISION_HUB_PUB_LEN  65
#define GW_PROVISION_OT_DATASET_MAX 254

#ifdef __cplusplus
extern "C" {
#endif

int  gw_provision_init(void);
bool gw_provision_is_complete(void);

const char    *gw_provision_get_ssid(void);
const char    *gw_provision_get_psk(void);
const char    *gw_provision_get_hub_mdns(void);
const uint8_t *gw_provision_get_hub_id(void);
const uint8_t *gw_provision_get_hub_pubkey(void);
const uint8_t *gw_provision_get_ot_dataset(size_t *out_len);

int gw_provision_set(const char *ssid, const char *psk,
		     const char *hub_mdns,
		     const uint8_t hub_id[GW_PROVISION_HUB_ID_LEN],
		     const uint8_t hub_pub[GW_PROVISION_HUB_PUB_LEN],
		     const uint8_t *ot_dataset,
		     size_t ot_dataset_len);

int gw_provision_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* FW_COMMON_GW_PROVISION_H_ */
