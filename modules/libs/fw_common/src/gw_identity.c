/* SPDX-License-Identifier: Apache-2.0 */

/*
 * gw_identity — P-256 keypair + deterministic-ECDSA signing for the
 * gateway, persisted via the fw_common kvstore abstraction.
 *
 * Same C source compiles under Zephyr (with tf-psa-crypto) and on
 * plain Linux (with mbedTLS PSA in libmbedcrypto).  Shell commands
 * for the Zephyr app live in apps/ncp_host_esp32c6/src/
 * gw_identity_shell.c — they use only the public API exposed here
 * plus apps-local proto_tcp helpers, so the core code stays
 * platform-neutral.
 *
 * Migration notes (vs. the original Zephyr-only version):
 *   - settings_subsys_init/settings_load_subtree → fw_kv_init +
 *     fw_kv_register + fw_kv_load_all
 *   - SETTINGS_STATIC_HANDLER_DEFINE → fw_kv_register() at init time
 *   - settings_save_one → fw_kv_save
 *   - LOG_* → fw_common/log.h (Zephyr LOG subsystem on Zephyr,
 *     stderr shim on Linux)
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <fw_common/gw_identity.h>
#include <fw_common/kvstore.h>
#include <fw_common/log.h>

#include <psa/crypto.h>

LOG_MODULE_REGISTER(gw_identity, LOG_LEVEL_INF);

/* RFC 6979 deterministic ECDSA — derives k from priv+message, no RNG
 * call per signature.  Sigs are byte-compatible with regular ECDSA
 * verify, so the policy alg stays DETERMINISTIC_ECDSA on both sign
 * and verify (a deterministic-ECDSA policy accepts any valid (R,S)). */
#define ECDSA_ALG       PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256)

/* ── state ─────────────────────────────────────────────────────────────── */

static uint8_t s_priv[GW_IDENTITY_PRIV_LEN];          /* 32-byte P-256 scalar */
static uint8_t s_pub[GW_IDENTITY_PUBKEY_LEN];          /* 0x04 || X[32] || Y[32] */
static uint8_t s_id[GW_IDENTITY_ID_LEN];
static bool    s_initialised;

/* ── NVS load callback (matches Zephyr settings handler contract) ────── */

static int gw_identity_kv_load_cb(const char *suffix,
				  const uint8_t *value, size_t value_len,
				  void *user)
{
	(void)user;
	if (strcmp(suffix, "p256_priv") == 0) {
		if (value_len != GW_IDENTITY_PRIV_LEN) {
			LOG_ERR("kv: p256_priv has bad len %zu", value_len);
			return -EINVAL;
		}
		memcpy(s_priv, value, GW_IDENTITY_PRIV_LEN);
		LOG_INF("P-256 priv loaded from NVS");
	}
	return 0;
}

/* ── PSA helpers ──────────────────────────────────────────────────────── */

static psa_status_t import_p256_priv(const uint8_t priv[GW_IDENTITY_PRIV_LEN],
				     psa_key_id_t *out_id,
				     psa_key_usage_t usage)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attr,
		PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_algorithm(&attr, ECDSA_ALG);
	psa_set_key_usage_flags(&attr, usage);
	return psa_import_key(&attr, priv, GW_IDENTITY_PRIV_LEN, out_id);
}

static psa_status_t import_p256_pub(const uint8_t pub[GW_IDENTITY_PUBKEY_LEN],
				    psa_key_id_t *out_id)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attr,
		PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_algorithm(&attr, ECDSA_ALG);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
	return psa_import_key(&attr, pub, GW_IDENTITY_PUBKEY_LEN, out_id);
}

/* gateway_id = first 8 bytes of SHA256(pubkey). */
static int derive_gateway_id(const uint8_t pub[GW_IDENTITY_PUBKEY_LEN],
			     uint8_t out_id[GW_IDENTITY_ID_LEN])
{
	uint8_t digest[32];
	size_t  digest_len = 0;

	psa_status_t rc = psa_hash_compute(PSA_ALG_SHA_256,
					   pub, GW_IDENTITY_PUBKEY_LEN,
					   digest, sizeof(digest),
					   &digest_len);
	if (rc != PSA_SUCCESS || digest_len != 32) {
		LOG_ERR("psa_hash_compute(SHA256) failed: %d", rc);
		return -EIO;
	}
	memcpy(out_id, digest, GW_IDENTITY_ID_LEN);
	return 0;
}

/* ── public API ────────────────────────────────────────────────────────── */

int gw_identity_init(void)
{
	if (s_initialised) return 0;

	psa_status_t rc = psa_crypto_init();
	if (rc != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init failed: %d", rc);
		return -EIO;
	}

	int err = fw_kv_init();
	if (err) {
		LOG_WRN("fw_kv_init: %d (continuing)", err);
	}
	err = fw_kv_register("gw_identity", gw_identity_kv_load_cb, NULL);
	if (err) {
		LOG_WRN("fw_kv_register: %d (continuing)", err);
	}
	err = fw_kv_load_all();
	if (err) {
		LOG_WRN("fw_kv_load_all: %d (continuing)", err);
	}

	bool have_saved_key = false;
	for (int i = 0; i < GW_IDENTITY_PRIV_LEN; i++) {
		if (s_priv[i] != 0) {
			have_saved_key = true;
			break;
		}
	}

	if (!have_saved_key) {
		psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
		psa_set_key_type(&attr,
			PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
		psa_set_key_bits(&attr, 256);
		psa_set_key_algorithm(&attr, ECDSA_ALG);
		psa_set_key_usage_flags(&attr,
			PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);

		psa_key_id_t key_id = PSA_KEY_ID_NULL;
		rc = psa_generate_key(&attr, &key_id);
		if (rc != PSA_SUCCESS) {
			LOG_ERR("psa_generate_key (P-256) failed: %d", rc);
			return -EIO;
		}

		size_t len = 0;
		rc = psa_export_key(key_id, s_priv, sizeof(s_priv), &len);
		if (rc != PSA_SUCCESS || len != GW_IDENTITY_PRIV_LEN) {
			psa_destroy_key(key_id);
			LOG_ERR("psa_export_key (priv) failed: %d (len=%zu)",
				rc, len);
			return -EIO;
		}
		rc = psa_export_public_key(key_id, s_pub, sizeof(s_pub), &len);
		psa_destroy_key(key_id);
		if (rc != PSA_SUCCESS || len != GW_IDENTITY_PUBKEY_LEN) {
			LOG_ERR("psa_export_public_key failed: %d (len=%zu)",
				rc, len);
			return -EIO;
		}

		err = fw_kv_save("gw_identity/p256_priv",
				 s_priv, sizeof(s_priv));
		if (err) {
			LOG_ERR("fw_kv_save: %d", err);
			return err;
		}
		LOG_INF("Generated fresh P-256 keypair (saved to NVS)");
	} else {
		psa_key_id_t key_id = PSA_KEY_ID_NULL;
		rc = import_p256_priv(s_priv, &key_id, PSA_KEY_USAGE_EXPORT);
		if (rc != PSA_SUCCESS) {
			LOG_ERR("import stored P-256 priv: %d", rc);
			return -EIO;
		}
		size_t len = 0;
		rc = psa_export_public_key(key_id, s_pub, sizeof(s_pub), &len);
		psa_destroy_key(key_id);
		if (rc != PSA_SUCCESS || len != GW_IDENTITY_PUBKEY_LEN) {
			LOG_ERR("export pub from loaded priv: %d (len=%zu)",
				rc, len);
			return -EIO;
		}
	}

	int rv = derive_gateway_id(s_pub, s_id);
	if (rv) return rv;

	LOG_INF("gateway_id = %02x%02x%02x%02x%02x%02x%02x%02x",
		s_id[0], s_id[1], s_id[2], s_id[3],
		s_id[4], s_id[5], s_id[6], s_id[7]);

	s_initialised = true;
	return 0;
}

const uint8_t *gw_identity_get_id(void)
{
	return s_initialised ? s_id : NULL;
}

const uint8_t *gw_identity_get_pubkey(void)
{
	return s_initialised ? s_pub : NULL;
}

int gw_identity_sign(void *ctx,
		     const uint8_t *msg, size_t msg_len,
		     uint8_t sig_out[64])
{
	(void)ctx;
	if (!s_initialised) return -ENODEV;

	psa_key_id_t key_id = PSA_KEY_ID_NULL;
	psa_status_t rc = import_p256_priv(s_priv, &key_id,
					   PSA_KEY_USAGE_SIGN_MESSAGE);
	if (rc != PSA_SUCCESS) {
		LOG_ERR("sign: import priv: %d", rc);
		return -EIO;
	}

	size_t sig_len = 0;
	rc = psa_sign_message(key_id, ECDSA_ALG,
			      msg, msg_len,
			      sig_out, GW_IDENTITY_SIG_LEN, &sig_len);
	psa_destroy_key(key_id);
	if (rc != PSA_SUCCESS || sig_len != GW_IDENTITY_SIG_LEN) {
		LOG_ERR("psa_sign_message: %d (sig_len=%zu)", rc, sig_len);
		return -EIO;
	}
	return 0;
}

int gw_identity_verify(void *ctx_pubkey,
		       const uint8_t *msg, size_t msg_len,
		       const uint8_t sig[64])
{
	const uint8_t *pub = ctx_pubkey;
	if (!pub) return -EINVAL;

	psa_key_id_t key_id = PSA_KEY_ID_NULL;
	psa_status_t rc = import_p256_pub(pub, &key_id);
	if (rc != PSA_SUCCESS) {
		LOG_ERR("verify: import pub: %d", rc);
		return -EIO;
	}
	rc = psa_verify_message(key_id, ECDSA_ALG,
				msg, msg_len, sig, 64);
	psa_destroy_key(key_id);
	if (rc != PSA_SUCCESS) {
		return -EBADMSG;
	}
	return 0;
}
