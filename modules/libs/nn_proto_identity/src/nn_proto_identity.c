/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <psa/crypto.h>

#include <nn_osal/osal.h>
#include <nn_proto_identity/nn_proto_identity.h>

#if defined(CONFIG_MBEDTLS_MEMORY_DEBUG)
#include <mbedtls/memory_buffer_alloc.h>
#endif

NN_OSAL_LOG_MODULE(nn_proto_identity);

/* RFC 6979 deterministic ECDSA — derives k from priv+message; no RNG
 * call per signature.  Sigs are byte-compatible with regular ECDSA
 * verify, so the policy alg stays DETERMINISTIC_ECDSA on both sides
 * (a deterministic-ECDSA policy accepts any valid (R,S) at verify). */
#define ECDSA_ALG  PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256)

static psa_status_t import_priv(const uint8_t priv[32],
				psa_key_id_t *out_id, psa_key_usage_t usage)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attr,
		PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_algorithm(&attr, ECDSA_ALG);
	psa_set_key_usage_flags(&attr, usage);
	return psa_import_key(&attr, priv, NN_PROTO_IDENTITY_PRIV_LEN, out_id);
}

static psa_status_t import_pub(const uint8_t pub[NN_PROTO_IDENTITY_PUBKEY_LEN],
			       psa_key_id_t *out_id)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attr,
		PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_algorithm(&attr, ECDSA_ALG);
	/* VERIFY_MESSAGE is meant to imply VERIFY_HASH per PSA spec, but
	 * TF-PSA-crypto's policy check doesn't auto-imply — pass both
	 * explicitly so psa_verify_hash works. */
	psa_set_key_usage_flags(&attr,
		PSA_KEY_USAGE_VERIFY_MESSAGE | PSA_KEY_USAGE_VERIFY_HASH);
	return psa_import_key(&attr, pub, NN_PROTO_IDENTITY_PUBKEY_LEN, out_id);
}

int nn_proto_identity_keygen(uint8_t priv[NN_PROTO_IDENTITY_PRIV_LEN],
			     uint8_t pub[NN_PROTO_IDENTITY_PUBKEY_LEN])
{
	if (!priv || !pub) {
		return -EINVAL;
	}
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attr,
		PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_algorithm(&attr, ECDSA_ALG);
	psa_set_key_usage_flags(&attr,
		PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_SIGN_HASH |
		PSA_KEY_USAGE_EXPORT);

	psa_key_id_t kid = PSA_KEY_ID_NULL;
	psa_status_t rc = psa_generate_key(&attr, &kid);
	if (rc != PSA_SUCCESS) {
		NN_LOG_ERR("psa_generate_key (P-256): %d", rc);
		return -EIO;
	}
	size_t len = 0;
	rc = psa_export_key(kid, priv, NN_PROTO_IDENTITY_PRIV_LEN, &len);
	if (rc != PSA_SUCCESS || len != NN_PROTO_IDENTITY_PRIV_LEN) {
		psa_destroy_key(kid);
		return -EIO;
	}
	rc = psa_export_public_key(kid, pub, NN_PROTO_IDENTITY_PUBKEY_LEN, &len);
	psa_destroy_key(kid);
	if (rc != PSA_SUCCESS || len != NN_PROTO_IDENTITY_PUBKEY_LEN) {
		return -EIO;
	}
	return 0;
}

int nn_proto_identity_pub_from_priv(const uint8_t priv[NN_PROTO_IDENTITY_PRIV_LEN],
				    uint8_t pub[NN_PROTO_IDENTITY_PUBKEY_LEN])
{
	if (!priv || !pub) {
		return -EINVAL;
	}
	psa_key_id_t kid = PSA_KEY_ID_NULL;
	psa_status_t rc = import_priv(priv, &kid, PSA_KEY_USAGE_EXPORT);
	if (rc != PSA_SUCCESS) {
		return -EIO;
	}
	size_t len = 0;
	rc = psa_export_public_key(kid, pub, NN_PROTO_IDENTITY_PUBKEY_LEN, &len);
	psa_destroy_key(kid);
	if (rc != PSA_SUCCESS || len != NN_PROTO_IDENTITY_PUBKEY_LEN) {
		return -EIO;
	}
	return 0;
}

int nn_proto_identity_derive_node_id(const uint8_t pub[NN_PROTO_IDENTITY_PUBKEY_LEN],
				     uint8_t out[NN_PROTO_IDENTITY_NODE_ID_LEN])
{
	uint8_t digest[32];
	size_t  digest_len = 0;
	psa_status_t rc = psa_hash_compute(PSA_ALG_SHA_256,
					   pub, NN_PROTO_IDENTITY_PUBKEY_LEN,
					   digest, sizeof(digest),
					   &digest_len);
	if (rc != PSA_SUCCESS || digest_len != 32) {
		return -EIO;
	}
	memcpy(out, digest, NN_PROTO_IDENTITY_NODE_ID_LEN);
	return 0;
}

/* TF-PSA-crypto is NOT compiled thread-safe in this build (no
 * CONFIG_MBEDTLS_THREADING_C).  Calling psa_* from multiple threads
 * concurrently can corrupt PSA's internal key-slot / heap state and
 * leave subsequent calls returning PSA_ERROR_INSUFFICIENT_MEMORY (-141)
 * permanently until the next reboot.  We observed this on the C6 with
 * three concurrent sign callers (deferred-logging thread for LOG_LINE,
 * shell/auto-engine threads, rx_thread for AUTO_ACK).
 *
 * This module mutex serializes every PSA entry from our identity code.
 * For total coverage, hub_crypto's PSA calls (ECIES decrypt) should
 * also acquire this lock — exposed via the `_psa_lock_*` API below. */
static K_MUTEX_DEFINE(s_psa_mutex);

void nn_proto_identity_psa_lock(void)   { k_mutex_lock(&s_psa_mutex, K_FOREVER); }
void nn_proto_identity_psa_unlock(void) { k_mutex_unlock(&s_psa_mutex); }

/* Key-id caches: import each priv/pub buffer into PSA only ONCE and reuse
 * the resulting key id across calls.  The previous design re-imported on
 * every sign/verify and called psa_destroy_key afterwards; TF-PSA-crypto
 * doesn't fully reclaim internal pool state across many import/destroy
 * cycles, so PSA_ERROR_INSUFFICIENT_MEMORY (-141) starts to appear after
 * ~17-20 ops (observed during OTA block downloads).  Caching breaks that
 * leak by removing the cycle entirely.
 *
 * The cache key is the caller-owned buffer ADDRESS — callers (nn_proto_
 * client, hub_crypto) hold these in long-lived static storage, so the
 * address is a stable identity.  If a different buffer comes in, evict
 * the old kid (destroy) and import the new. */
#define SIGN_KEY_CACHE_SIZE 2
#define VERIFY_KEY_CACHE_SIZE 4

struct kid_cache_entry {
	const uint8_t *buf_addr;     /* NULL = slot free */
	psa_key_id_t   kid;
};

static struct kid_cache_entry s_sign_cache[SIGN_KEY_CACHE_SIZE];
static struct kid_cache_entry s_verify_cache[VERIFY_KEY_CACHE_SIZE];

/* Caller must hold s_psa_mutex.  Returns kid for `priv`, importing
 * + caching on first sight.  Returns PSA_KEY_ID_NULL on failure. */
static psa_key_id_t get_or_import_priv_kid(const uint8_t priv[32])
{
	for (size_t i = 0; i < SIGN_KEY_CACHE_SIZE; i++) {
		if (s_sign_cache[i].buf_addr == priv) {
			return s_sign_cache[i].kid;
		}
	}
	psa_key_id_t kid = PSA_KEY_ID_NULL;
	/* SIGN_MESSAGE is meant to imply SIGN_HASH per PSA spec, but
	 * TF-PSA-crypto's policy check doesn't auto-imply — pass both
	 * explicitly so the hash-then-sign path works. */
	psa_status_t rc = import_priv(priv, &kid,
				      PSA_KEY_USAGE_SIGN_MESSAGE |
				      PSA_KEY_USAGE_SIGN_HASH);
	if (rc != PSA_SUCCESS) {
		return PSA_KEY_ID_NULL;
	}
	/* Find a free slot, or evict the oldest (slot 0). */
	for (size_t i = 0; i < SIGN_KEY_CACHE_SIZE; i++) {
		if (s_sign_cache[i].buf_addr == NULL) {
			s_sign_cache[i].buf_addr = priv;
			s_sign_cache[i].kid      = kid;
			return kid;
		}
	}
	/* Evict slot 0 (LRU approximation — sign callers in this codebase
	 * are very few, ≤ SIGN_KEY_CACHE_SIZE in practice, so eviction is
	 * rare).  Shift the rest down. */
	(void)psa_destroy_key(s_sign_cache[0].kid);
	for (size_t i = 0; i < SIGN_KEY_CACHE_SIZE - 1; i++) {
		s_sign_cache[i] = s_sign_cache[i + 1];
	}
	s_sign_cache[SIGN_KEY_CACHE_SIZE - 1].buf_addr = priv;
	s_sign_cache[SIGN_KEY_CACHE_SIZE - 1].kid      = kid;
	return kid;
}

static psa_key_id_t get_or_import_pub_kid(const uint8_t pub[NN_PROTO_IDENTITY_PUBKEY_LEN])
{
	for (size_t i = 0; i < VERIFY_KEY_CACHE_SIZE; i++) {
		if (s_verify_cache[i].buf_addr == pub) {
			return s_verify_cache[i].kid;
		}
	}
	psa_key_id_t kid = PSA_KEY_ID_NULL;
	psa_status_t rc = import_pub(pub, &kid);
	if (rc != PSA_SUCCESS) {
		return PSA_KEY_ID_NULL;
	}
	for (size_t i = 0; i < VERIFY_KEY_CACHE_SIZE; i++) {
		if (s_verify_cache[i].buf_addr == NULL) {
			s_verify_cache[i].buf_addr = pub;
			s_verify_cache[i].kid      = kid;
			return kid;
		}
	}
	/* Evict slot 0 — LRU approximation. */
	(void)psa_destroy_key(s_verify_cache[0].kid);
	for (size_t i = 0; i < VERIFY_KEY_CACHE_SIZE - 1; i++) {
		s_verify_cache[i] = s_verify_cache[i + 1];
	}
	s_verify_cache[VERIFY_KEY_CACHE_SIZE - 1].buf_addr = pub;
	s_verify_cache[VERIFY_KEY_CACHE_SIZE - 1].kid      = kid;
	return kid;
}

/* Sign / verify use psa_hash_compute + psa_sign_hash (NOT
 * psa_sign_message).  Rationale:
 *
 *   `psa_sign_message` internally allocates a SHA-256 ctx + the
 *   deterministic-ECDSA HMAC workspace + the ECDSA sign workspace
 *   all SIMULTANEOUSLY, then frees them at the end.  TF-PSA-crypto's
 *   internal allocator fragments badly under sustained sign load,
 *   and that combined transient peak (~3-4 KB) is what was hitting
 *   PSA_ERROR_INSUFFICIENT_MEMORY (-141) on the C6 mid-OTA.
 *
 *   Splitting into hash-then-sign keeps each allocation smaller and
 *   serialised: the SHA-256 ctx (~200 B) is allocated, finalised, and
 *   freed BEFORE the ECDSA workspace is allocated.  Same algebraic
 *   result (deterministic ECDSA over SHA-256) but much friendlier to
 *   the mbedtls allocator.  See feedback_psa_141_partial_mitigations.md.
 *
 * Key policy unchanged: PSA_KEY_USAGE_SIGN_MESSAGE implies SIGN_HASH,
 * and the algorithm PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256)
 * accepts both call shapes.  Same on the verify side.
 */
int nn_proto_identity_sign(void *ctx_priv32,
			   const uint8_t *msg, size_t msg_len,
			   uint8_t sig_out[64])
{
	const uint8_t *priv = ctx_priv32;
	if (!priv) {
		return -EINVAL;
	}

	/* Serialize PSA — TF-PSA-crypto is not thread-safe in this build
	 * (see banner above s_psa_mutex). */
	k_mutex_lock(&s_psa_mutex, K_FOREVER);

	psa_key_id_t kid = get_or_import_priv_kid(priv);
	if (kid == PSA_KEY_ID_NULL) {
		k_mutex_unlock(&s_psa_mutex);
		printk("identity_sign: import_priv failed\n");
		return -EIO;
	}

	uint8_t hash[32];
	size_t  hash_len = 0;
	psa_status_t rc = psa_hash_compute(PSA_ALG_SHA_256,
					   msg, msg_len,
					   hash, sizeof(hash), &hash_len);
	if (rc != PSA_SUCCESS || hash_len != sizeof(hash)) {
		k_mutex_unlock(&s_psa_mutex);
		printk("identity_sign: psa_hash_compute=%d len=%zu\n",
		       (int)rc, hash_len);
		return -EIO;
	}

	size_t sig_len = 0;
	rc = psa_sign_hash(kid, ECDSA_ALG, hash, hash_len,
			   sig_out, NN_PROTO_IDENTITY_SIG_LEN, &sig_len);

#if defined(CONFIG_MBEDTLS_MEMORY_DEBUG)
	/* Periodic heap snapshot to root-cause PSA -141 INSUFFICIENT_MEMORY.
	 * Dump every 16 signs and on every failure so we can correlate
	 * heap state with the failure. */
	static uint32_t s_sign_count;
	static bool     s_status_dumped;
	bool dump = (rc != PSA_SUCCESS) || ((++s_sign_count & 0x0f) == 0);
	if (dump) {
		size_t cur_used = 0, cur_blocks = 0;
		size_t max_used = 0, max_blocks = 0;
		size_t allocs = 0, frees = 0;
		mbedtls_memory_buffer_alloc_cur_get(&cur_used, &cur_blocks);
		mbedtls_memory_buffer_alloc_max_get(&max_used, &max_blocks);
		mbedtls_memory_buffer_alloc_count_get(&allocs, &frees);
		printk("mbedtls_heap[#%u rc=%d]: cur=%zu/%zu (used/blks) "
		       "max=%zu/%zu allocs=%zu frees=%zu leak=%ld\n",
		       (unsigned)s_sign_count, (int)rc,
		       cur_used, cur_blocks, max_used, max_blocks,
		       allocs, frees, (long)(allocs - frees));
	}
	/* On the very FIRST failure, dump the full block list — this shows
	 * each free segment's size so we can see if the heap is fragmented
	 * into pieces too small for the next ECDSA workspace request. */
	if (rc != PSA_SUCCESS && !s_status_dumped) {
		printk("=== mbedtls_memory_buffer_alloc_status (first -141) ===\n");
		mbedtls_memory_buffer_alloc_status();
		printk("=== end status ===\n");
		s_status_dumped = true;
	}
#endif

	k_mutex_unlock(&s_psa_mutex);

	if (rc != PSA_SUCCESS || sig_len != NN_PROTO_IDENTITY_SIG_LEN) {
		printk("identity_sign: psa_sign_hash=%d sig_len=%zu\n",
		       (int)rc, sig_len);
		return -EIO;
	}
	return 0;
}

int nn_proto_identity_verify(void *ctx_pubkey,
			     const uint8_t *msg, size_t msg_len,
			     const uint8_t sig[64])
{
	const uint8_t *pub = ctx_pubkey;
	if (!pub) {
		return -EINVAL;
	}
	/* Same serialization + hash-then-verify rationale as sign. */
	k_mutex_lock(&s_psa_mutex, K_FOREVER);

	psa_key_id_t kid = get_or_import_pub_kid(pub);
	if (kid == PSA_KEY_ID_NULL) {
		k_mutex_unlock(&s_psa_mutex);
		return -EBADMSG;
	}

	uint8_t hash[32];
	size_t  hash_len = 0;
	psa_status_t rc = psa_hash_compute(PSA_ALG_SHA_256,
					   msg, msg_len,
					   hash, sizeof(hash), &hash_len);
	if (rc == PSA_SUCCESS && hash_len == sizeof(hash)) {
		rc = psa_verify_hash(kid, ECDSA_ALG, hash, hash_len, sig, 64);
	}

	k_mutex_unlock(&s_psa_mutex);
	if (rc != PSA_SUCCESS) {
		return -EBADMSG;
	}
	return 0;
}
