/* SPDX-License-Identifier: Apache-2.0 */

#include <node_mgr/nn_session_boot.h>
#include <node_mgr/nn_proto_client.h>
#include <fw_common/hub_crypto.h>
#include <fw_common/nn_session.h>
#include <node_mgr/nn_group.h>

#include <nn_osal/osal.h>
#include <nn_proto/nn_proto.h>

#include <errno.h>
#include <string.h>
#include <psa/crypto.h>
#include <nn_proto_identity/nn_proto_identity.h>

NN_OSAL_LOG_MODULE(nn_session_boot);

static uint8_t       s_dev_salt[NN_SESSION_SALT_LEN];
static bool          s_have_salt;
static nn_session_t  s_sess;
static uint8_t       s_hub_salt[NN_SESSION_SALT_LEN];
static bool          s_have_hub_salt;
static bool          s_probe_acked;
static nn_osal_mutex_t s_sess_lock;
static bool            s_lock_ready;

/* Fixed probe the hub knows out-of-band (see proto_router _on_sess_probe). */
static const uint8_t PROBE[16] = "nn-sess-probe!!!";

static void sess_lock(void)
{
	if (!s_lock_ready) {
		nn_osal_mutex_init(&s_sess_lock);
		s_lock_ready = true;
	}
	nn_osal_mutex_lock(&s_sess_lock, -1);
}

static void sess_unlock(void)
{
	nn_osal_mutex_unlock(&s_sess_lock);
}

int nn_session_boot_salt_raw(uint8_t out[8])
{
	if (!s_have_salt) {
		return -1;
	}
	memcpy(out, s_dev_salt, NN_SESSION_SALT_LEN);
	return 0;
}

int nn_session_boot_salt_hex(char out[17])
{
	if (!s_have_salt) {
		out[0] = '\0';
		return -1;
	}
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < NN_SESSION_SALT_LEN; i++) {
		out[2 * i]     = hx[s_dev_salt[i] >> 4];
		out[2 * i + 1] = hx[s_dev_salt[i] & 0xF];
	}
	out[16] = '\0';
	return 0;
}

static void on_probe_result(nn_proto_reliable_result_t result,
			    uint32_t tid, uint32_t rtt_ms,
			    const uint8_t *body, size_t body_len, void *user)
{
	ARG_UNUSED(tid); ARG_UNUSED(body); ARG_UNUSED(body_len);
	ARG_UNUSED(user);
	if (result == NN_PROTO_RELIABLE_OK) {
		s_probe_acked = true;
		NN_LOG_INF("session VERIFIED by hub (probe acked, rtt=%u ms)",
			   rtt_ms);
	} else {
		NN_LOG_WRN("session probe gave up (rv=%d)", (int)result);
	}
}

/* Seal + send the probe via the reliable NOSIGN path: retransmits until
 * the hub's SESS_PROBE_ACK arrives, each attempt re-sending the SAME
 * sealed record (hub acks replays of a verified probe too). */
static void send_probe(void)
{
	uint8_t aad[2] = {
		(uint8_t)(NN_PROTO_CMD_SESS_PROBE & 0xFF),
		(uint8_t)(NN_PROTO_CMD_SESS_PROBE >> 8),
	};
	uint8_t rec[NN_SESSION_CTR_LEN + sizeof PROBE + NN_SESSION_TAG_LEN];
	size_t  rec_len = 0;

	sess_lock();
	int rv = s_sess.established
		? nn_session_seal(&s_sess, aad, sizeof aad, PROBE, sizeof PROBE,
				  rec, sizeof rec, &rec_len)
		: -ENOTCONN;
	sess_unlock();
	if (rv) {
		NN_LOG_WRN("probe seal rv=%d", rv);
		return;
	}
	rv = nn_proto_client_send_d2h_reliable_ns(NN_PROTO_CMD_SESS_PROBE,
						  NN_PROTO_CMD_SESS_PROBE_ACK,
						  0, rec, rec_len,
						  on_probe_result, NULL);
	NN_LOG_INF("SESS_PROBE enqueued (%zu B) rv=%d", rec_len, rv);
}

/* H2D SESS_INIT: body = 8-byte hub salt. */
static void on_sess_init(uint32_t tid, const uint8_t *body, size_t body_len,
			 void *user)
{
	ARG_UNUSED(tid); ARG_UNUSED(user);
	if (body_len != NN_SESSION_SALT_LEN) {
		NN_LOG_WRN("SESS_INIT bad len %zu", body_len);
		return;
	}

	/* Same hub salt again (hub re-sent because our probe was lost)?
	 * Do NOT re-derive — that would reset our tx counter and every
	 * new record would land inside the hub's replay window.  Just
	 * re-probe with the next counter. */
	if (s_sess.established && s_have_hub_salt &&
	    memcmp(body, s_hub_salt, NN_SESSION_SALT_LEN) == 0) {
		NN_LOG_INF("SESS_INIT repeat — re-probing");
		send_probe();
		return;
	}

	uint8_t ecdh[32];
	int rv = hub_crypto_static_ecdh(ecdh);
	if (rv) {
		NN_LOG_WRN("SESS_INIT: static ECDH rv=%d", rv);
		return;
	}
	sess_lock();
	rv = nn_session_derive(&s_sess, ecdh, s_dev_salt, body, true);
	sess_unlock();
	memset(ecdh, 0, sizeof ecdh);
	if (rv) {
		NN_LOG_WRN("SESS_INIT: derive rv=%d", rv);
		return;
	}
	memcpy(s_hub_salt, body, NN_SESSION_SALT_LEN);
	s_have_hub_salt = true;
	s_probe_acked = false;
	NN_LOG_INF("session derived (device side) — probing");
	send_probe();
}

/* H2D SESS_GROUP_KEY: session-SEALED [epoch:4 LE][key:32].  Deliver the
 * Phase-4 cascade group key.  AAD binds cmd+tid like every sealed cmd. */
static void on_group_key(uint32_t tid, const uint8_t *body, size_t body_len,
			 void *user)
{
	ARG_UNUSED(user);
	uint8_t aad[6] = {
		(uint8_t)(NN_PROTO_CMD_SESS_GROUP_KEY & 0xFF),
		(uint8_t)(NN_PROTO_CMD_SESS_GROUP_KEY >> 8),
	};
	aad[2] = (uint8_t)(tid & 0xFF);
	aad[3] = (uint8_t)((tid >> 8) & 0xFF);
	aad[4] = (uint8_t)((tid >> 16) & 0xFF);
	aad[5] = (uint8_t)((tid >> 24) & 0xFF);

	uint8_t plain[4 + NN_GROUP_KEY_LEN];
	size_t  plain_len = 0;
	int rv = nn_session_boot_open(aad, sizeof aad, body, body_len,
				      plain, sizeof plain, &plain_len);
	if (rv == -EEXIST) {
		return;                       /* re-delivered record */
	}
	if (rv != 0 || plain_len != sizeof plain) {
		NN_LOG_WRN("GROUP_KEY open rv=%d len=%zu", rv, plain_len);
		return;
	}
	uint32_t epoch = nn_osal_get_le32(plain);
	(void)nn_group_set(epoch, plain + 4);
	memset(plain, 0, sizeof plain);
}

/* ── SESS_HELLO announcer ────────────────────────────────────────────
 * The salt used to ride the 537-byte INFO_REPLY, which 6LoWPAN
 * fragments and weak links then drop wholesale.  Instead we announce
 * the salt in a ~90-byte unsigned frame until the handshake completes.
 * Called from the heartbeat worker (no dedicated thread — SRAM is
 * full to the last KB; the heartbeat context already survives larger
 * signed sends, so stack headroom is proven). */

void nn_session_boot_hello_tick(void)
{
	/* Keep announcing until the FULL chain lands: session derived AND
	 * the Phase-4 group key installed.  A repeat HELLO makes the hub
	 * re-send SESS_INIT -> we re-probe -> hub re-acks AND re-pushes
	 * the group key — so a lost GROUP_KEY frame self-heals. */
	if ((s_sess.established && nn_group_ready()) || !s_have_salt) {
		return;
	}
	int rv = nn_proto_client_send_d2h_reply_ns(
		NN_PROTO_CMD_SESS_HELLO, 0,
		s_dev_salt, NN_SESSION_SALT_LEN);
	if (rv == 0) {
		NN_LOG_INF("SESS_HELLO sent");
	}
}

int nn_session_boot_init(void)
{
	if (!s_lock_ready) {
		nn_osal_mutex_init(&s_sess_lock);
		s_lock_ready = true;
	}
	nn_proto_identity_psa_lock();
	psa_status_t rc = psa_generate_random(s_dev_salt, sizeof s_dev_salt);
	nn_proto_identity_psa_unlock();
	if (rc != PSA_SUCCESS) {
		NN_LOG_ERR("salt gen failed: %d", (int)rc);
		return -EIO;
	}
	s_have_salt = true;

	int rv = nn_proto_client_register_h2d_handler(NN_PROTO_CMD_SESS_INIT,
						      on_sess_init, NULL);
	if (rv) {
		NN_LOG_WRN("register SESS_INIT handler rv=%d", rv);
	}
	rv = nn_proto_client_register_h2d_handler(NN_PROTO_CMD_SESS_GROUP_KEY,
						  on_group_key, NULL);
	if (rv) {
		NN_LOG_WRN("register GROUP_KEY handler rv=%d", rv);
	}
	NN_LOG_INF("session boot: salt minted, announcing via SESS_HELLO");
	return rv;
}

bool nn_session_boot_ready(void)
{
	sess_lock();
	bool up = s_sess.established;
	sess_unlock();
	return up;
}

int nn_session_boot_seal(const uint8_t *aad, size_t aad_len,
			 const uint8_t *pt, size_t pt_len,
			 uint8_t *out, size_t out_cap, size_t *out_len)
{
	sess_lock();
	int rv = s_sess.established
		? nn_session_seal(&s_sess, aad, aad_len, pt, pt_len,
				  out, out_cap, out_len)
		: -ENOTCONN;
	sess_unlock();
	return rv;
}

int nn_session_boot_open(const uint8_t *aad, size_t aad_len,
			 const uint8_t *in, size_t in_len,
			 uint8_t *out, size_t out_cap, size_t *out_len)
{
	sess_lock();
	int rv = s_sess.established
		? nn_session_open(&s_sess, aad, aad_len, in, in_len,
				  out, out_cap, out_len)
		: -ENOTCONN;
	sess_unlock();
	return rv;
}
