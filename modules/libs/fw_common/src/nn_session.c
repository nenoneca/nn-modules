/* SPDX-License-Identifier: Apache-2.0 */

#include <fw_common/nn_session.h>

#include <errno.h>
#include <string.h>

#include <psa/crypto.h>
#include <nn_proto_identity/nn_proto_identity.h>   /* PSA serialization lock */

/* HKDF-SHA256(ecdh, salt, info) -> a persistent AES-256-GCM PSA key.
 * The derivation streams straight into an AES key object (never a raw
 * byte buffer), so key material doesn't sit in RAM, and the key is
 * imported ONCE per session — per-message import cost ~600 us on the C6,
 * far more than the AES-GCM itself. */
static int hkdf_aes_key(const uint8_t ecdh[32],
			const uint8_t *salt, size_t salt_len,
			const uint8_t *info, size_t info_len,
			psa_key_id_t *out_id)
{
	psa_key_id_t ikm_id = PSA_KEY_ID_NULL;
	psa_key_attributes_t iattr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&iattr, PSA_KEY_TYPE_DERIVE);
	psa_set_key_algorithm(&iattr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_usage_flags(&iattr, PSA_KEY_USAGE_DERIVE);
	if (psa_import_key(&iattr, ecdh, 32, &ikm_id) != PSA_SUCCESS) {
		return -EIO;
	}

	int rv = -EIO;
	psa_key_derivation_operation_t kdf = PSA_KEY_DERIVATION_OPERATION_INIT;
	if (psa_key_derivation_setup(&kdf, PSA_ALG_HKDF(PSA_ALG_SHA_256))
	    != PSA_SUCCESS) {
		goto out_ikm;
	}
	if (psa_key_derivation_input_bytes(&kdf,
		PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len) != PSA_SUCCESS ||
	    psa_key_derivation_input_key(&kdf,
		PSA_KEY_DERIVATION_INPUT_SECRET, ikm_id) != PSA_SUCCESS ||
	    psa_key_derivation_input_bytes(&kdf,
		PSA_KEY_DERIVATION_INPUT_INFO, info, info_len) != PSA_SUCCESS) {
		goto out_kdf;
	}
	psa_key_attributes_t aattr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&aattr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&aattr, 256);
	psa_set_key_algorithm(&aattr, PSA_ALG_GCM);
	psa_set_key_usage_flags(&aattr,
		PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
	if (psa_key_derivation_output_key(&aattr, &kdf, out_id) == PSA_SUCCESS) {
		rv = 0;
	}
out_kdf:
	psa_key_derivation_abort(&kdf);
out_ikm:
	psa_destroy_key(ikm_id);
	return rv;
}

void nn_session_free(nn_session_t *s)
{
	if (!s) {
		return;
	}
	nn_proto_identity_psa_lock();
	if (s->tx_key) {
		psa_destroy_key((psa_key_id_t)s->tx_key);
	}
	if (s->rx_key) {
		psa_destroy_key((psa_key_id_t)s->rx_key);
	}
	nn_proto_identity_psa_unlock();
	s->tx_key = 0;
	s->rx_key = 0;
	s->established = false;
}

int nn_session_derive(nn_session_t *s,
		      const uint8_t ecdh[32],
		      const uint8_t dev_salt[NN_SESSION_SALT_LEN],
		      const uint8_t hub_salt[NN_SESSION_SALT_LEN],
		      bool is_device)
{
	if (!s || !ecdh || !dev_salt || !hub_salt) {
		return -EINVAL;
	}
	nn_session_free(s);   /* idempotent re-derive */

	uint8_t salt[2 * NN_SESSION_SALT_LEN];
	memcpy(salt, dev_salt, NN_SESSION_SALT_LEN);
	memcpy(salt + NN_SESSION_SALT_LEN, hub_salt, NN_SESSION_SALT_LEN);

	/* Two directional keys.  The device sends on d2h, receives on h2d;
	 * the hub is mirrored — so device.tx_key material == hub.rx_key. */
	static const uint8_t info_d2h[] = "nn-sess-v1/d2h";
	static const uint8_t info_h2d[] = "nn-sess-v1/h2d";

	psa_key_id_t k_d2h = PSA_KEY_ID_NULL, k_h2d = PSA_KEY_ID_NULL;
	nn_proto_identity_psa_lock();
	int rv = hkdf_aes_key(ecdh, salt, sizeof salt,
			      info_d2h, sizeof info_d2h - 1, &k_d2h);
	if (rv) {
		nn_proto_identity_psa_unlock();
		return rv;
	}
	rv = hkdf_aes_key(ecdh, salt, sizeof salt,
			  info_h2d, sizeof info_h2d - 1, &k_h2d);
	if (rv) {
		psa_destroy_key(k_d2h);
		nn_proto_identity_psa_unlock();
		return rv;
	}
	nn_proto_identity_psa_unlock();

	if (is_device) {
		s->tx_key = (uint32_t)k_d2h;
		s->rx_key = (uint32_t)k_h2d;
	} else {
		s->tx_key = (uint32_t)k_h2d;
		s->rx_key = (uint32_t)k_d2h;
	}
	s->ctr_tx = 0;
	s->ctr_rx_hi = 0;
	s->rx_window = 0;
	s->rx_any = false;
	s->established = true;
	return 0;
}

static void ctr_to_nonce(uint64_t ctr, uint8_t nonce[12])
{
	memset(nonce, 0, 4);
	for (int i = 0; i < 8; i++) {
		nonce[4 + i] = (uint8_t)(ctr >> (56 - 8 * i));  /* BE */
	}
}

int nn_session_seal(nn_session_t *s,
		    const uint8_t *aad, size_t aad_len,
		    const uint8_t *pt, size_t pt_len,
		    uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (!s || !s->established || !out || !out_len) {
		return -EINVAL;
	}
	if (out_cap < NN_SESSION_CTR_LEN + pt_len + NN_SESSION_TAG_LEN) {
		return -ENOSPC;
	}
	uint64_t ctr = s->ctr_tx;
	uint8_t nonce[12];
	ctr_to_nonce(ctr, nonce);

	/* Emit the counter big-endian so the peer rebuilds the nonce. */
	for (int i = 0; i < 8; i++) {
		out[i] = (uint8_t)(ctr >> (56 - 8 * i));
	}
	size_t ct_len = 0;
	nn_proto_identity_psa_lock();
	psa_status_t e = psa_aead_encrypt((psa_key_id_t)s->tx_key, PSA_ALG_GCM,
					  nonce, 12, aad, aad_len, pt, pt_len,
					  out + NN_SESSION_CTR_LEN,
					  out_cap - NN_SESSION_CTR_LEN, &ct_len);
	nn_proto_identity_psa_unlock();
	if (e != PSA_SUCCESS) {
		return -EIO;
	}
	s->ctr_tx++;
	*out_len = NN_SESSION_CTR_LEN + ct_len;
	return 0;
}

int nn_session_open(nn_session_t *s,
		    const uint8_t *aad, size_t aad_len,
		    const uint8_t *in, size_t in_len,
		    uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (!s || !s->established || !in || !out || !out_len) {
		return -EINVAL;
	}
	if (in_len < NN_SESSION_CTR_LEN + NN_SESSION_TAG_LEN) {
		return -EBADMSG;
	}
	uint64_t ctr = 0;
	for (int i = 0; i < 8; i++) {
		ctr = (ctr << 8) | in[i];
	}
	/* Sliding-window anti-replay (64 deep).  Strict monotonic would
	 * deadlock a delayed reply overtaken by a newer one: its counter
	 * is fixed across retransmits, so it could never be delivered.
	 * Window checks happen BEFORE the (cheap) decrypt; the window is
	 * only advanced AFTER the tag verifies, so a forged counter can't
	 * wedge the guard. */
	if (s->rx_any) {
		if (ctr <= s->ctr_rx_hi) {
			uint64_t off = s->ctr_rx_hi - ctr;
			if (off >= 64 || (s->rx_window >> off) & 1u) {
				return -EEXIST;   /* too old, or seen */
			}
		}
	}
	uint8_t nonce[12];
	ctr_to_nonce(ctr, nonce);

	nn_proto_identity_psa_lock();
	psa_status_t e = psa_aead_decrypt((psa_key_id_t)s->rx_key, PSA_ALG_GCM,
					  nonce, 12, aad, aad_len,
					  in + NN_SESSION_CTR_LEN,
					  in_len - NN_SESSION_CTR_LEN,
					  out, out_cap, out_len);
	nn_proto_identity_psa_unlock();
	if (e == PSA_ERROR_INVALID_SIGNATURE) {
		return -EBADMSG;
	}
	if (e != PSA_SUCCESS) {
		return -EIO;
	}
	if (!s->rx_any || ctr > s->ctr_rx_hi) {
		uint64_t shift = s->rx_any ? (ctr - s->ctr_rx_hi) : 0;
		s->rx_window = (shift >= 64) ? 0 : (s->rx_window << shift);
		s->rx_window |= 1u;
		s->ctr_rx_hi = ctr;
		s->rx_any = true;
	} else {
		s->rx_window |= (1ull << (s->ctr_rx_hi - ctr));
	}
	return 0;
}
