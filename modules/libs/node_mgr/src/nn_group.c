/* SPDX-License-Identifier: Apache-2.0 */

#include <node_mgr/nn_group.h>

#include <nn_osal/osal.h>
#include <errno.h>
#include <string.h>

#include <psa/crypto.h>
#include <nn_proto_identity/nn_proto_identity.h>   /* PSA serialization lock */

NN_OSAL_LOG_MODULE(nn_group);

struct group_slot {
	uint32_t     epoch;
	psa_key_id_t key;       /* imported once; 0 = empty */
};

static struct group_slot s_cur, s_prev;
static uint64_t s_ctr;                 /* (boot_rand32 << 32) | seq */
static nn_osal_mutex_t s_lock;
static bool s_lock_ready;

struct peer_replay {
	uint8_t  id[8];
	uint64_t last_ctr;
	bool     used;
};
static struct peer_replay s_peers[4];

static void lock(void)
{
	if (!s_lock_ready) {
		nn_osal_mutex_init(&s_lock);
		s_lock_ready = true;
	}
	nn_osal_mutex_lock(&s_lock, -1);
}

static void unlock(void)
{
	nn_osal_mutex_unlock(&s_lock);
}

static int import_group_key(const uint8_t key[NN_GROUP_KEY_LEN],
			    psa_key_id_t *out_id)
{
	psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&a, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&a, 256);
	psa_set_key_algorithm(&a, PSA_ALG_GCM);
	psa_set_key_usage_flags(&a,
		PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
	return psa_import_key(&a, key, NN_GROUP_KEY_LEN, out_id)
		== PSA_SUCCESS ? 0 : -EIO;
}

int nn_group_set(uint32_t epoch, const uint8_t key[NN_GROUP_KEY_LEN])
{
	lock();
	if (s_cur.key && s_cur.epoch == epoch) {
		unlock();
		return 0;             /* re-delivery of the same epoch */
	}
	nn_proto_identity_psa_lock();
	if (s_prev.key) {
		psa_destroy_key(s_prev.key);
		s_prev.key = 0;
	}
	s_prev = s_cur;               /* keep one older epoch alive */
	s_cur.key = 0;
	int rv = import_group_key(key, &s_cur.key);
	nn_proto_identity_psa_unlock();
	if (rv == 0) {
		s_cur.epoch = epoch;
		if (s_ctr == 0) {
			uint32_t r = 0;
			nn_proto_identity_psa_lock();
			psa_generate_random((uint8_t *)&r, sizeof r);
			nn_proto_identity_psa_unlock();
			s_ctr = ((uint64_t)r << 32);
		}
		NN_LOG_INF("group key installed (epoch %u)", epoch);
	} else {
		NN_LOG_WRN("group key import rv=%d", rv);
	}
	unlock();
	return rv;
}

bool nn_group_ready(void)
{
	lock();
	bool up = (s_cur.key != 0);
	unlock();
	return up;
}

static void build_nonce(const uint8_t id8[8], uint64_t ctr, uint8_t nonce[12])
{
	memcpy(nonce, id8, 4);
	for (int i = 0; i < 8; i++) {
		nonce[4 + i] = (uint8_t)(ctr >> (56 - 8 * i));
	}
}

static void build_aad(uint16_t cmd, const uint8_t id8[8], uint8_t aad[10])
{
	aad[0] = (uint8_t)(cmd & 0xFF);
	aad[1] = (uint8_t)(cmd >> 8);
	memcpy(aad + 2, id8, 8);
}

int nn_group_seal(const uint8_t self_id8[8], uint16_t cmd,
		  const uint8_t *pt, size_t pt_len,
		  uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (!self_id8 || !out || !out_len) {
		return -EINVAL;
	}
	if (out_cap < NN_GROUP_HDR_LEN + pt_len + NN_GROUP_TAG_LEN) {
		return -ENOSPC;
	}
	lock();
	if (!s_cur.key) {
		unlock();
		return -ENOTCONN;
	}
	uint64_t ctr = ++s_ctr;
	uint32_t epoch = s_cur.epoch;
	psa_key_id_t key = s_cur.key;
	unlock();

	uint8_t nonce[12], aad[10];
	build_nonce(self_id8, ctr, nonce);
	build_aad(cmd, self_id8, aad);

	nn_osal_put_le32(epoch, out);
	for (int i = 0; i < 8; i++) {
		out[4 + i] = (uint8_t)(ctr >> (56 - 8 * i));
	}
	size_t ct_len = 0;
	nn_proto_identity_psa_lock();
	psa_status_t e = psa_aead_encrypt(key, PSA_ALG_GCM, nonce, 12,
					  aad, sizeof aad, pt, pt_len,
					  out + NN_GROUP_HDR_LEN,
					  out_cap - NN_GROUP_HDR_LEN, &ct_len);
	nn_proto_identity_psa_unlock();
	if (e != PSA_SUCCESS) {
		return -EIO;
	}
	*out_len = NN_GROUP_HDR_LEN + ct_len;
	return 0;
}

int nn_group_open(const uint8_t sender_id8[8], uint16_t cmd,
		  const uint8_t *in, size_t in_len,
		  uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (!sender_id8 || !in || !out || !out_len) {
		return -EINVAL;
	}
	if (in_len < NN_GROUP_OVERHEAD) {
		return -EBADMSG;
	}
	uint32_t epoch = nn_osal_get_le32(in);
	uint64_t ctr = 0;
	for (int i = 0; i < 8; i++) {
		ctr = (ctr << 8) | in[4 + i];
	}

	lock();
	psa_key_id_t key = 0;
	if (s_cur.key && s_cur.epoch == epoch) {
		key = s_cur.key;
	} else if (s_prev.key && s_prev.epoch == epoch) {
		key = s_prev.key;
	}
	if (!key) {
		unlock();
		return -EINVAL;       /* unknown epoch */
	}
	/* Per-sender replay: strictly increasing counter, except a
	 * changed hi-word (sender rebooted → fresh random stream). */
	struct peer_replay *pr = NULL;
	for (int i = 0; i < (int)ARRAY_SIZE(s_peers); i++) {
		if (s_peers[i].used &&
		    memcmp(s_peers[i].id, sender_id8, 8) == 0) {
			pr = &s_peers[i];
			break;
		}
	}
	if (pr && (ctr >> 32) == (pr->last_ctr >> 32) &&
	    ctr <= pr->last_ctr) {
		unlock();
		return -EEXIST;
	}
	unlock();

	uint8_t nonce[12], aad[10];
	build_nonce(sender_id8, ctr, nonce);
	build_aad(cmd, sender_id8, aad);

	nn_proto_identity_psa_lock();
	psa_status_t e = psa_aead_decrypt(key, PSA_ALG_GCM, nonce, 12,
					  aad, sizeof aad,
					  in + NN_GROUP_HDR_LEN,
					  in_len - NN_GROUP_HDR_LEN,
					  out, out_cap, out_len);
	nn_proto_identity_psa_unlock();
	if (e == PSA_ERROR_INVALID_SIGNATURE) {
		return -EBADMSG;
	}
	if (e != PSA_SUCCESS) {
		return -EIO;
	}

	lock();
	if (!pr) {
		for (int i = 0; i < (int)ARRAY_SIZE(s_peers); i++) {
			if (!s_peers[i].used) {
				pr = &s_peers[i];
				break;
			}
		}
		if (!pr) {
			pr = &s_peers[0];   /* evict arbitrarily */
		}
		memcpy(pr->id, sender_id8, 8);
		pr->used = true;
		pr->last_ctr = 0;
	}
	if (ctr > pr->last_ctr || (ctr >> 32) != (pr->last_ctr >> 32)) {
		pr->last_ctr = ctr;
	}
	unlock();
	return 0;
}
