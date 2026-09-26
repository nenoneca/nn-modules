/* SPDX-License-Identifier: Apache-2.0 */

/*
 * hub_crypto — device-side ECIES v2 over PSA (X25519 + HKDF-SHA256 +
 * AES-256-GCM).  This file lives in fw_common so both the Zephyr
 * firmware tree AND the pure-Linux gateway daemon (host/gw_linux/)
 * link the same logic.
 *
 * Platform-specific dependencies were factored into thin abstractions
 * that have Zephyr and Linux backends:
 *   - <fw_common/kvstore.h>  for NVS persistence (settings on Zephyr,
 *                             one file per key on Linux)
 *   - <fw_common/base64.h>   RFC 4648 (single source, no platform deps)
 *   - <fw_common/envelope.h> hand-rolled JSON envelope parser/builder
 *   - <fw_common/log.h>      LOG_* macros (Zephyr LOG subsystem on
 *                             Zephyr, stderr-based shim on Linux)
 *
 * Random nonces use psa_generate_random(), which is supported by both
 * tf-psa-crypto on Zephyr and mbedTLS 4.x on Linux — so no separate
 * random shim.
 *
 * Protocol (unchanged from the original Zephyr-only version):
 *   shared_e = ECDH(ephem_priv, peer_static_pub)
 *   shared_s = ECDH(own_static_priv, peer_static_pub)
 *   aes_key  = HKDF-SHA256(shared_e || shared_s, salt=epk, info=dir_tag)
 *   ct       = AES-256-GCM(aes_key, nonce, plaintext)
 */

#include <fw_common/hub_crypto.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <fw_common/base64.h>
#include <fw_common/envelope.h>
#include <fw_common/kvstore.h>
#include <fw_common/log.h>
#include <nn_proto_identity/nn_proto_identity.h>   /* PSA serialization lock */

#include <psa/crypto.h>

LOG_MODULE_REGISTER(hub_crypto, LOG_LEVEL_INF);

/* ── HKDF direction tags ──────────────────────────────────────────────────── */

static const uint8_t INFO_H2D[] = "nn-hub-v2-h2d";
static const uint8_t INFO_D2H[] = "nn-hub-v2-d2h";
#define INFO_H2D_LEN  (sizeof(INFO_H2D) - 1)
#define INFO_D2H_LEN  (sizeof(INFO_D2H) - 1)

/* ── Persistent state ─────────────────────────────────────────────────────── */

static uint8_t g_dev_x25519_priv[32];   /* device long-term X25519 private key */
static uint8_t g_dev_x25519_pub[32];    /* corresponding public key */
static uint8_t g_hub_x25519_pub[32];    /* hub long-term X25519 public key */
static bool    g_dev_key_ready;
static bool    g_hub_key_ready;

/* ── NVS load callback (invoked by fw_kv_load_all) ───────────────────────── */

static int hub_crypto_kv_load_cb(const char *suffix,
				 const uint8_t *value, size_t value_len,
				 void *user)
{
	(void)user;
	if (!strcmp(suffix, "dev_x25519_priv")) {
		if (value_len != 32) {
			return -EINVAL;
		}
		memcpy(g_dev_x25519_priv, value, 32);
		LOG_INF("Device X25519 private key loaded from NVS");
	} else if (!strcmp(suffix, "hub_x25519_pub")) {
		if (value_len != 32) {
			return -EINVAL;
		}
		memcpy(g_hub_x25519_pub, value, 32);
		g_hub_key_ready = true;
		LOG_INF("Hub X25519 public key loaded from NVS");
	}
	return 0;
}

/* ── Internal PSA helpers (unchanged from the Zephyr-only version) ─────── */

static psa_status_t import_x25519_priv(const uint8_t priv[32],
				       psa_key_id_t *out_id)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attr,
		PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
	psa_set_key_bits(&attr, 255);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
	return psa_import_key(&attr, priv, 32, out_id);
}

static psa_status_t derive_aes_key(const uint8_t *ikm,   size_t ikm_len,
				    const uint8_t epk[32],
				    const uint8_t *info, size_t info_len,
				    psa_key_id_t *out_id)
{
	psa_status_t rc;
	psa_key_id_t hkdf_key_id = PSA_KEY_ID_NULL;

	psa_key_attributes_t hattr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&hattr, PSA_KEY_TYPE_DERIVE);
	psa_set_key_algorithm(&hattr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_usage_flags(&hattr, PSA_KEY_USAGE_DERIVE);

	rc = psa_import_key(&hattr, ikm, ikm_len, &hkdf_key_id);
	if (rc != PSA_SUCCESS) return rc;

	psa_key_derivation_operation_t kdf = PSA_KEY_DERIVATION_OPERATION_INIT;
	rc = psa_key_derivation_setup(&kdf, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	if (rc != PSA_SUCCESS) goto cleanup_hkdf_key;

	rc = psa_key_derivation_input_bytes(&kdf,
		PSA_KEY_DERIVATION_INPUT_SALT, epk, 32);
	if (rc != PSA_SUCCESS) goto cleanup_op;

	rc = psa_key_derivation_input_key(&kdf,
		PSA_KEY_DERIVATION_INPUT_SECRET, hkdf_key_id);
	if (rc != PSA_SUCCESS) goto cleanup_op;

	rc = psa_key_derivation_input_bytes(&kdf,
		PSA_KEY_DERIVATION_INPUT_INFO, info, info_len);
	if (rc != PSA_SUCCESS) goto cleanup_op;

	psa_key_attributes_t aattr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&aattr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&aattr, 256);
	psa_set_key_algorithm(&aattr, PSA_ALG_GCM);
	psa_set_key_usage_flags(&aattr,
		PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);

	rc = psa_key_derivation_output_key(&aattr, &kdf, out_id);

cleanup_op:
	psa_key_derivation_abort(&kdf);
cleanup_hkdf_key:
	psa_destroy_key(hkdf_key_id);
	return rc;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int hub_crypto_init(void)
{
	/* kvstore subsystem must be ready before we ask to load keys. */
	int rc_io = fw_kv_init();
	if (rc_io) {
		LOG_WRN("fw_kv_init: %d", rc_io);
	}
	rc_io = fw_kv_register("hub_crypto", hub_crypto_kv_load_cb, NULL);
	if (rc_io) {
		LOG_WRN("fw_kv_register: %d", rc_io);
	}
	rc_io = fw_kv_load_all();
	if (rc_io) {
		LOG_WRN("fw_kv_load_all: %d", rc_io);
	}

	psa_status_t rc = psa_crypto_init();
	if (rc != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init failed: %d", rc);
		return -EIO;
	}

	bool have_saved_key = false;
	for (int i = 0; i < 32; i++) {
		if (g_dev_x25519_priv[i] != 0) {
			have_saved_key = true;
			break;
		}
	}

	if (have_saved_key) {
		psa_key_id_t key_id = PSA_KEY_ID_NULL;
		rc = import_x25519_priv(g_dev_x25519_priv, &key_id);
		if (rc != PSA_SUCCESS) {
			LOG_ERR("Failed to import stored X25519 private key: %d", rc);
			return -EIO;
		}
		size_t pub_len;
		rc = psa_export_public_key(key_id, g_dev_x25519_pub, 32, &pub_len);
		psa_destroy_key(key_id);
		if (rc != PSA_SUCCESS) {
			LOG_ERR("Failed to export device X25519 pub: %d", rc);
			return -EIO;
		}
		g_dev_key_ready = true;
		LOG_INF("Device X25519 key loaded — pub: %02x%02x%02x%02x...",
			g_dev_x25519_pub[0], g_dev_x25519_pub[1],
			g_dev_x25519_pub[2], g_dev_x25519_pub[3]);
	} else {
		psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
		psa_set_key_type(&attr,
			PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
		psa_set_key_bits(&attr, 255);
		psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
		psa_set_key_usage_flags(&attr,
			PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);

		psa_key_id_t key_id = PSA_KEY_ID_NULL;
		rc = psa_generate_key(&attr, &key_id);
		if (rc != PSA_SUCCESS) {
			LOG_ERR("psa_generate_key (X25519) failed: %d", rc);
			return -EIO;
		}

		size_t len;
		rc = psa_export_key(key_id, g_dev_x25519_priv, 32, &len);
		if (rc != PSA_SUCCESS || len != 32) {
			psa_destroy_key(key_id);
			LOG_ERR("Failed to export X25519 private key: %d", rc);
			return -EIO;
		}

		rc = psa_export_public_key(key_id, g_dev_x25519_pub, 32, &len);
		psa_destroy_key(key_id);
		if (rc != PSA_SUCCESS || len != 32) {
			LOG_ERR("Failed to export X25519 public key: %d", rc);
			return -EIO;
		}

		int err = fw_kv_save("hub_crypto/dev_x25519_priv",
				     g_dev_x25519_priv, 32);
		if (err) {
			LOG_WRN("Failed to save X25519 key: %d", err);
		}

		g_dev_key_ready = true;
		LOG_INF("New X25519 key generated — pub: %02x%02x%02x%02x...",
			g_dev_x25519_pub[0], g_dev_x25519_pub[1],
			g_dev_x25519_pub[2], g_dev_x25519_pub[3]);
	}

	return 0;
}

void hub_crypto_get_device_x25519_pub(uint8_t out[32])
{
	memcpy(out, g_dev_x25519_pub, 32);
}

int hub_crypto_static_ecdh(uint8_t out[32])
{
	bool have_priv = false, have_hub = false;
	for (int i = 0; i < 32; i++) {
		have_priv |= (g_dev_x25519_priv[i] != 0);
		have_hub  |= (g_hub_x25519_pub[i]  != 0);
	}
	if (!have_priv || !have_hub) {
		return -EINVAL;  /* key(s) unset */
	}

	/* PSA here is NOT thread-safe — serialize with the same identity
	 * lock every other PSA caller uses.  Unlocked concurrent PSA (this
	 * ran on the rx thread while the heartbeat thread signed) wedged
	 * the crypto internals and permanently starved the OT rx path
	 * (net_pkt pool exhaustion — the 0.0.15 fleet-wide H2D outage). */
	nn_proto_identity_psa_lock();
	psa_key_id_t kid = PSA_KEY_ID_NULL;
	psa_status_t rc = import_x25519_priv(g_dev_x25519_priv, &kid);
	if (rc != PSA_SUCCESS) {
		nn_proto_identity_psa_unlock();
		return -EIO;
	}
	size_t olen = 0;
	rc = psa_raw_key_agreement(PSA_ALG_ECDH, kid,
				   g_hub_x25519_pub, 32,
				   out, 32, &olen);
	psa_destroy_key(kid);
	nn_proto_identity_psa_unlock();
	return (rc == PSA_SUCCESS && olen == 32) ? 0 : -EIO;
}

int hub_crypto_set_hub_pubkey(const uint8_t hub_x25519_pub[32])
{
	memcpy(g_hub_x25519_pub, hub_x25519_pub, 32);
	g_hub_key_ready = true;

	int err = fw_kv_save("hub_crypto/hub_x25519_pub",
			     g_hub_x25519_pub, 32);
	if (err) {
		LOG_WRN("Failed to save hub X25519 pub: %d", err);
	}
	LOG_INF("Hub X25519 pub stored — %02x%02x%02x%02x...",
		hub_x25519_pub[0], hub_x25519_pub[1],
		hub_x25519_pub[2], hub_x25519_pub[3]);
	return 0;
}

static int hub_crypto_decrypt_locked(char *json_in, uint8_t *plain_out,
				     size_t *plain_len)
{
	if (!g_dev_key_ready) {
		LOG_ERR("Device key not ready");
		return -EACCES;
	}
	if (!g_hub_key_ready) {
		LOG_ERR("Hub key not received yet (BLE provisioning pending)");
		return -EACCES;
	}

	/* ── Parse JSON envelope ──────────────────────────────────────────── */
	struct fw_envelope_v2 env;
	if (fw_envelope_v2_parse(json_in, &env) != 0) {
		LOG_ERR("Bad envelope");
		return -EBADMSG;
	}

	/* ── Base64-decode ciphertext (epk + nonce came out raw) ───────────── */
	uint8_t ct_buf[512];
	size_t  ct_len;
	if (fw_base64_decode(ct_buf, sizeof(ct_buf), &ct_len,
			     env.ct_b64, env.ct_b64_len) != 0) {
		LOG_ERR("ciphertext base64 decode failed");
		return -EINVAL;
	}

	/* ── Compute IKM = shared_e || shared_s (h2d) ────────────────────── */
	uint8_t ikm[64];
	psa_status_t prc;
	psa_key_id_t dev_key_id = PSA_KEY_ID_NULL;
	size_t shared_len;

	prc = import_x25519_priv(g_dev_x25519_priv, &dev_key_id);
	if (prc != PSA_SUCCESS) return -EIO;

	prc = psa_raw_key_agreement(PSA_ALG_ECDH, dev_key_id,
				    env.epk, 32,
				    ikm, 32, &shared_len);
	if (prc != PSA_SUCCESS || shared_len != 32) {
		psa_destroy_key(dev_key_id);
		LOG_ERR("ECDH(dev, epk) failed: %d", prc);
		return -EIO;
	}

	prc = psa_raw_key_agreement(PSA_ALG_ECDH, dev_key_id,
				    g_hub_x25519_pub, 32,
				    ikm + 32, 32, &shared_len);
	psa_destroy_key(dev_key_id);
	if (prc != PSA_SUCCESS || shared_len != 32) {
		LOG_ERR("ECDH(dev, hub_static) failed: %d", prc);
		return -EIO;
	}

	/* ── Derive AES-256-GCM key ───────────────────────────────────────── */
	psa_key_id_t aes_key_id = PSA_KEY_ID_NULL;
	prc = derive_aes_key(ikm, 64, env.epk, INFO_H2D, INFO_H2D_LEN,
			     &aes_key_id);
	memset(ikm, 0, sizeof(ikm));
	if (prc != PSA_SUCCESS) {
		LOG_ERR("derive_aes_key failed: %d", prc);
		return -EIO;
	}

	/* ── AES-256-GCM decrypt ──────────────────────────────────────────── */
	prc = psa_aead_decrypt(aes_key_id, PSA_ALG_GCM,
			       env.nonce, 12,
			       NULL, 0,
			       ct_buf, ct_len,
			       plain_out, *plain_len, plain_len);
	psa_destroy_key(aes_key_id);

	if (prc != PSA_SUCCESS) {
		LOG_ERR("AES-GCM decrypt failed: %d (auth failure?)", prc);
		return -EACCES;
	}
	return 0;
}

/* Public wrapper — serializes PSA against nn_proto_identity callers
 * (sign/verify on a different thread).  See nn_proto_identity.h for
 * rationale: TF-PSA-crypto is not thread-safe in this build, and
 * concurrent psa_* corrupts internal state. */
int hub_crypto_decrypt(char *json_in, uint8_t *plain_out, size_t *plain_len)
{
	nn_proto_identity_psa_lock();
	int rv = hub_crypto_decrypt_locked(json_in, plain_out, plain_len);
	nn_proto_identity_psa_unlock();
	return rv;
}

static int hub_crypto_encrypt_locked(const uint8_t *plain, size_t plain_len,
				     char *json_out, size_t json_cap)
{
	if (!g_dev_key_ready) {
		LOG_ERR("Device key not ready");
		return -EACCES;
	}
	if (!g_hub_key_ready) {
		LOG_ERR("Hub key not received yet");
		return -EACCES;
	}

	/* ── Generate ephemeral X25519 key pair ───────────────────────────── */
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attr,
		PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
	psa_set_key_bits(&attr, 255);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
	psa_set_key_usage_flags(&attr,
		PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);

	psa_key_id_t ephem_key_id = PSA_KEY_ID_NULL;
	psa_status_t prc = psa_generate_key(&attr, &ephem_key_id);
	if (prc != PSA_SUCCESS) return -EIO;

	uint8_t epk[32];
	size_t  epk_len;
	prc = psa_export_public_key(ephem_key_id, epk, 32, &epk_len);
	if (prc != PSA_SUCCESS || epk_len != 32) {
		psa_destroy_key(ephem_key_id);
		return -EIO;
	}

	uint8_t ephem_priv[32];
	size_t  ephem_priv_len;
	prc = psa_export_key(ephem_key_id, ephem_priv, 32, &ephem_priv_len);
	psa_destroy_key(ephem_key_id);
	if (prc != PSA_SUCCESS || ephem_priv_len != 32) return -EIO;

	/* ── Compute IKM = shared_e || shared_s (d2h) ────────────────────── */
	uint8_t ikm[64];
	psa_key_id_t ephem_id2 = PSA_KEY_ID_NULL;
	psa_key_id_t static_id = PSA_KEY_ID_NULL;
	size_t shared_len;

	prc = import_x25519_priv(ephem_priv, &ephem_id2);
	if (prc != PSA_SUCCESS) goto cleanup_ephem_priv;
	prc = import_x25519_priv(g_dev_x25519_priv, &static_id);
	if (prc != PSA_SUCCESS) goto cleanup_ephem_id2;

	prc = psa_raw_key_agreement(PSA_ALG_ECDH, ephem_id2,
				    g_hub_x25519_pub, 32,
				    ikm, 32, &shared_len);
	if (prc != PSA_SUCCESS || shared_len != 32) goto cleanup_both;

	prc = psa_raw_key_agreement(PSA_ALG_ECDH, static_id,
				    g_hub_x25519_pub, 32,
				    ikm + 32, 32, &shared_len);
	if (prc != PSA_SUCCESS || shared_len != 32) goto cleanup_both;

cleanup_both:
	psa_destroy_key(static_id);
cleanup_ephem_id2:
	psa_destroy_key(ephem_id2);
cleanup_ephem_priv:
	memset(ephem_priv, 0, sizeof(ephem_priv));

	if (prc != PSA_SUCCESS) {
		LOG_ERR("ECDH failed: %d", prc);
		return -EIO;
	}

	/* ── Derive AES-256-GCM key ───────────────────────────────────────── */
	psa_key_id_t aes_key_id = PSA_KEY_ID_NULL;
	prc = derive_aes_key(ikm, 64, epk, INFO_D2H, INFO_D2H_LEN, &aes_key_id);
	memset(ikm, 0, sizeof(ikm));
	if (prc != PSA_SUCCESS) {
		LOG_ERR("derive_aes_key failed: %d", prc);
		return -EIO;
	}

	/* ── AES-256-GCM encrypt ──────────────────────────────────────────── */
	uint8_t nonce[12];
	prc = psa_generate_random(nonce, sizeof(nonce));
	if (prc != PSA_SUCCESS) {
		psa_destroy_key(aes_key_id);
		LOG_ERR("psa_generate_random: %d", prc);
		return -EIO;
	}

	uint8_t ct_buf[256 + 16];
	size_t  ct_len;

	prc = psa_aead_encrypt(aes_key_id, PSA_ALG_GCM,
			       nonce, 12,
			       NULL, 0,
			       plain, plain_len,
			       ct_buf, sizeof(ct_buf), &ct_len);
	psa_destroy_key(aes_key_id);
	if (prc != PSA_SUCCESS) {
		LOG_ERR("AES-GCM encrypt failed: %d", prc);
		return -EIO;
	}

	/* ── Build JSON envelope using fw_common envelope builder ────────── */
	int rc = fw_envelope_v2_build(json_out, json_cap,
				      epk, nonce, ct_buf, ct_len);
	if (rc != 0) {
		LOG_ERR("envelope build failed: %d (cap=%zu)", rc, json_cap);
		return rc;
	}
	return 0;
}

/* Public wrapper — same PSA serialization rationale as decrypt. */
int hub_crypto_encrypt(const uint8_t *plain, size_t plain_len,
		       char *json_out, size_t json_cap)
{
	nn_proto_identity_psa_lock();
	int rv = hub_crypto_encrypt_locked(plain, plain_len, json_out, json_cap);
	nn_proto_identity_psa_unlock();
	return rv;
}
