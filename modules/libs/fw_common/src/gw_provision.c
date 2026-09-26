/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <fw_common/gw_provision.h>
#include <fw_common/kvstore.h>
#include <fw_common/log.h>

LOG_MODULE_REGISTER(gw_provision, LOG_LEVEL_INF);

#define KV_PREFIX "gw_provision"

static struct {
	char    ssid    [GW_PROVISION_SSID_MAX];
	char    psk     [GW_PROVISION_PSK_MAX];
	char    hub_mdns[GW_PROVISION_HUB_MDNS_MAX];
	uint8_t hub_id  [GW_PROVISION_HUB_ID_LEN];
	uint8_t hub_pub [GW_PROVISION_HUB_PUB_LEN];
	uint8_t ot_dataset[GW_PROVISION_OT_DATASET_MAX];
	uint16_t ot_dataset_len;

	bool ssid_set;
	bool psk_set;
	bool hub_mdns_set;
	bool hub_id_set;
	bool hub_pub_set;
	bool ot_dataset_set;
} P;

static int load_cb(const char *suffix, const uint8_t *value, size_t len,
		   void *user)
{
	(void)user;
	if (!strcmp(suffix, "ssid")) {
		if (len > GW_PROVISION_SSID_MAX - 1) return -EINVAL;
		memcpy(P.ssid, value, len);
		P.ssid[len] = '\0';
		P.ssid_set = true;
	} else if (!strcmp(suffix, "psk")) {
		if (len > GW_PROVISION_PSK_MAX - 1) return -EINVAL;
		memcpy(P.psk, value, len);
		P.psk[len] = '\0';
		P.psk_set = true;
	} else if (!strcmp(suffix, "hub_mdns")) {
		if (len > GW_PROVISION_HUB_MDNS_MAX - 1) return -EINVAL;
		memcpy(P.hub_mdns, value, len);
		P.hub_mdns[len] = '\0';
		P.hub_mdns_set = true;
	} else if (!strcmp(suffix, "hub_id")) {
		if (len != GW_PROVISION_HUB_ID_LEN) return -EINVAL;
		memcpy(P.hub_id, value, GW_PROVISION_HUB_ID_LEN);
		P.hub_id_set = true;
	} else if (!strcmp(suffix, "hub_pub")) {
		if (len != GW_PROVISION_HUB_PUB_LEN) return -EINVAL;
		memcpy(P.hub_pub, value, GW_PROVISION_HUB_PUB_LEN);
		P.hub_pub_set = true;
	} else if (!strcmp(suffix, "ot_dataset")) {
		if (len == 0 || len > GW_PROVISION_OT_DATASET_MAX) return -EINVAL;
		memcpy(P.ot_dataset, value, len);
		P.ot_dataset_len = (uint16_t)len;
		P.ot_dataset_set = true;
	}
	return 0;
}

int gw_provision_init(void)
{
	int err = fw_kv_init();
	if (err && err != -EALREADY) {
		LOG_ERR("fw_kv_init: %d", err);
		return err;
	}
	err = fw_kv_register(KV_PREFIX, load_cb, NULL);
	if (err) {
		LOG_ERR("fw_kv_register: %d", err);
		return err;
	}
	err = fw_kv_load_all();
	if (err) {
		LOG_WRN("fw_kv_load_all: %d (continuing)", err);
	}

	if (gw_provision_is_complete()) {
		LOG_INF("loaded: ssid=\"%s\" hub_mdns=\"%s\" hub_id="
			"%02x%02x%02x%02x%02x%02x%02x%02x",
			P.ssid, P.hub_mdns,
			P.hub_id[0], P.hub_id[1], P.hub_id[2], P.hub_id[3],
			P.hub_id[4], P.hub_id[5], P.hub_id[6], P.hub_id[7]);
	} else {
		LOG_WRN("not provisioned — waiting for BLE provisioning");
	}
	return 0;
}

bool gw_provision_is_complete(void)
{
	return P.ssid_set && P.psk_set && P.hub_mdns_set &&
	       P.hub_id_set && P.hub_pub_set && P.ot_dataset_set;
}

const char *gw_provision_get_ssid(void)
{
	return P.ssid_set ? P.ssid : NULL;
}

const char *gw_provision_get_psk(void)
{
	return P.psk_set ? P.psk : NULL;
}

const char *gw_provision_get_hub_mdns(void)
{
	return P.hub_mdns_set ? P.hub_mdns : NULL;
}

const uint8_t *gw_provision_get_hub_id(void)
{
	return P.hub_id_set ? P.hub_id : NULL;
}

const uint8_t *gw_provision_get_hub_pubkey(void)
{
	return P.hub_pub_set ? P.hub_pub : NULL;
}

const uint8_t *gw_provision_get_ot_dataset(size_t *out_len)
{
	if (!P.ot_dataset_set) {
		if (out_len) *out_len = 0;
		return NULL;
	}
	if (out_len) *out_len = P.ot_dataset_len;
	return P.ot_dataset;
}

int gw_provision_set(const char *ssid, const char *psk,
		     const char *hub_mdns,
		     const uint8_t hub_id[GW_PROVISION_HUB_ID_LEN],
		     const uint8_t hub_pub[GW_PROVISION_HUB_PUB_LEN],
		     const uint8_t *ot_dataset,
		     size_t ot_dataset_len)
{
	if (!ssid || !psk || !hub_mdns || !hub_id || !hub_pub || !ot_dataset) {
		return -EINVAL;
	}
	size_t lssid = strlen(ssid);
	size_t lpsk  = strlen(psk);
	size_t lhost = strlen(hub_mdns);
	if (lssid == 0 || lssid >= GW_PROVISION_SSID_MAX ||
	    lpsk  == 0 || lpsk  >= GW_PROVISION_PSK_MAX ||
	    lhost == 0 || lhost >= GW_PROVISION_HUB_MDNS_MAX ||
	    ot_dataset_len == 0 ||
	    ot_dataset_len > GW_PROVISION_OT_DATASET_MAX) {
		return -EINVAL;
	}

	int err;
	err = fw_kv_save(KV_PREFIX "/ssid",     ssid,     lssid);
	if (err) return err;
	err = fw_kv_save(KV_PREFIX "/psk",      psk,      lpsk);
	if (err) return err;
	err = fw_kv_save(KV_PREFIX "/hub_mdns", hub_mdns, lhost);
	if (err) return err;
	err = fw_kv_save(KV_PREFIX "/hub_id",   hub_id,
			 GW_PROVISION_HUB_ID_LEN);
	if (err) return err;
	err = fw_kv_save(KV_PREFIX "/hub_pub",  hub_pub,
			 GW_PROVISION_HUB_PUB_LEN);
	if (err) return err;
	err = fw_kv_save(KV_PREFIX "/ot_dataset", ot_dataset, ot_dataset_len);
	if (err) return err;

	memcpy(P.ssid,     ssid,     lssid);  P.ssid[lssid] = '\0';
	memcpy(P.psk,      psk,      lpsk);   P.psk[lpsk]   = '\0';
	memcpy(P.hub_mdns, hub_mdns, lhost);  P.hub_mdns[lhost] = '\0';
	memcpy(P.hub_id,   hub_id,  GW_PROVISION_HUB_ID_LEN);
	memcpy(P.hub_pub,  hub_pub, GW_PROVISION_HUB_PUB_LEN);
	memcpy(P.ot_dataset, ot_dataset, ot_dataset_len);
	P.ot_dataset_len = (uint16_t)ot_dataset_len;
	P.ssid_set = P.psk_set = P.hub_mdns_set = true;
	P.hub_id_set = P.hub_pub_set = P.ot_dataset_set = true;
	return 0;
}

int gw_provision_clear(void)
{
	static const char * const keys[] = {
		KV_PREFIX "/ssid", KV_PREFIX "/psk",
		KV_PREFIX "/hub_mdns",
		KV_PREFIX "/hub_id", KV_PREFIX "/hub_pub",
		KV_PREFIX "/ot_dataset",
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		(void)fw_kv_delete(keys[i]);
	}
	memset(&P, 0, sizeof(P));
	return 0;
}
