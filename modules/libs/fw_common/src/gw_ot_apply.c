/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <fw_common/gw_ot_apply.h>
#include <fw_common/log.h>
#include <fw_common/ncp_link.h>
#include <fw_common/spinel.h>

LOG_MODULE_REGISTER(gw_ot_apply, LOG_LEVEL_INF);

#define SET_TIMEOUT_MS  2000u
/* Dataset writes make the NCP rewrite its settings flash (each PHY/MAC/NET
 * property change clears and re-stores the active dataset): measured
 * 2202 ms per write, every write, on a C6 that had a stored dataset --
 * a freshly flashed one answers at once, which is why the plain 2 s
 * timeout looked fine in the first bring-up and then failed on the
 * first write for every re-provision after it. */
#define DATASET_SET_TIMEOUT_MS  15000u
/* An NCP that was attached (a stored dataset auto-starts Thread on boot)
 * detaches after STACK_UP=0; wait for it to report so before writing
 * PHY/MAC/NET properties.  Bounded: a Thread detach is seconds. */
#define DETACH_WAIT_MS  15000u
#define DETACH_POLL_MS  250u

/* OT operational dataset TLV types (Thread 1.x §8.10). */
#define TLV_CHANNEL           0
#define TLV_PANID             1
#define TLV_EXT_PANID         2
#define TLV_NETWORK_NAME      3
#define TLV_PSKC              4
#define TLV_NETWORK_KEY       5
#define TLV_MESH_LOCAL_PREFIX 7
#define TLV_SECURITY_POLICY  12
#define TLV_ACTIVE_TIMESTAMP 14
#define TLV_CHANNEL_MASK     53

/* clock_gettime/nanosleep: POSIX, and what proto_router already treats as
 * platform-neutral in this library. */
static void sleep_ms(unsigned ms)
{
	struct timespec ts = { .tv_sec = ms / 1000u,
			       .tv_nsec = (long)(ms % 1000u) * 1000000L };
	nanosleep(&ts, NULL);
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Wait until NET_ROLE reads DETACHED or DISABLED.  Returns the last role
 * seen; the caller applies anyway on timeout (the sets then fail loudly,
 * which beats hiding the state). */
static int wait_detached(void)
{
	uint64_t t0 = now_ms();
	int role = -1;
	for (;;) {
		uint8_t r = 0xff;
		size_t len = sizeof r;
		if (ncp_link_get(SPINEL_PROP_NET_ROLE, &r, &len, SET_TIMEOUT_MS) == 0 &&
		    len == 1) {
			role = r;
			if (r == SPINEL_NET_ROLE_DETACHED || r == SPINEL_NET_ROLE_DISABLED)
				return role;
		}
		if (now_ms() - t0 >= DETACH_WAIT_MS) {
			LOG_WRN("NCP still role=%d after %u ms -- applying anyway",
				role, DETACH_WAIT_MS);
			return role;
		}
		sleep_ms(DETACH_POLL_MS);
	}
}

static int set_raw(const char *label, uint32_t prop,
		   const uint8_t *v, size_t len)
{
	uint32_t ls = 0;
	uint64_t t0 = now_ms();
	int rv = ncp_link_set_raw(prop, v, len, &ls, DATASET_SET_TIMEOUT_MS);
	unsigned took = (unsigned)(now_ms() - t0);
	if (rv < 0) {
		LOG_ERR("set %s (prop 0x%02x) failed after %u ms: rv=%d ls=%u",
			label, prop, took, rv, ls);
		return rv;
	}
	LOG_INF("set %s (prop 0x%02x, %zu B): ok in %u ms", label, prop, len, took);
	return 0;
}

static int apply_channel(const uint8_t *v, uint8_t len)
{
	if (len != 3) { LOG_WRN("Channel TLV bad len %u", len); return -EINVAL; }
	uint16_t ch = ((uint16_t)v[1] << 8) | v[2];
	uint8_t  c  = (uint8_t)(ch & 0xff);
	return set_raw("PHY_CHAN", SPINEL_PROP_PHY_CHAN, &c, 1);
}

static int apply_panid(const uint8_t *v, uint8_t len)
{
	if (len != 2) { LOG_WRN("PanID TLV bad len %u", len); return -EINVAL; }
	uint8_t le[2] = { v[1], v[0] };
	return set_raw("MAC_PANID", SPINEL_PROP_MAC_15_4_PANID, le, 2);
}

static int apply_xpanid(const uint8_t *v, uint8_t len)
{
	if (len != 8) { LOG_WRN("ExtPanID TLV bad len %u", len); return -EINVAL; }
	return set_raw("NET_XPANID", SPINEL_PROP_NET_XPANID, v, 8);
}

static int apply_network_name(const uint8_t *v, uint8_t len)
{
	if (len == 0 || len > 16) { LOG_WRN("NetName TLV bad len %u", len); return -EINVAL; }
	uint8_t buf[17];
	memcpy(buf, v, len);
	buf[len] = '\0';
	return set_raw("NET_NETWORK_NAME", SPINEL_PROP_NET_NETWORK_NAME,
		       buf, (size_t)len + 1);
}

static int apply_network_key(const uint8_t *v, uint8_t len)
{
	if (len != 16) { LOG_WRN("NetKey TLV bad len %u", len); return -EINVAL; }
	return set_raw("NET_NETWORK_KEY", SPINEL_PROP_NET_NETWORK_KEY, v, 16);
}

static int apply_mesh_local_prefix(const uint8_t *v, uint8_t len)
{
	if (len != 8) { LOG_WRN("ML-Prefix TLV bad len %u", len); return -EINVAL; }
	uint8_t buf[17] = { 0 };
	memcpy(buf, v, 8);
	buf[16] = 64;
	return set_raw("IPV6_ML_PREFIX", SPINEL_PROP_IPV6_ML_PREFIX, buf, 17);
}

/* Does the NCP already hold this dataset's network identity?  Reads
 * PAN-ID / extended PAN-ID back and compares with the TLVs.  The CHANNEL is
 * deliberately not compared: the network migrates channels on its own
 * (pending dataset from the leader), after which the host's stored dataset
 * still names the old channel -- re-applying it would drag this radio back
 * onto the channel the whole mesh just left.
 * Writing them again is not free: every write cycles the NCP's stored
 * dataset and the caller has to take the stack DOWN first, which drops
 * every child this router has -- so a plain gateway-host restart used to
 * orphan the sensors for minutes. */
static bool ncp_already_configured(const uint8_t *tlvs, size_t tlvs_len)
{
	int have = 0, match = 0;
	size_t i = 0;
	while (i + 2 <= tlvs_len) {
		uint8_t t = tlvs[i], l = tlvs[i + 1];
		if (i + 2 + l > tlvs_len) return false;
		const uint8_t *v = &tlvs[i + 2];
		uint8_t got[8]; size_t gl;
		switch (t) {
		case TLV_PANID:
			if (l != 2) return false;
			have++; gl = sizeof got;
			if (ncp_link_get(SPINEL_PROP_MAC_15_4_PANID, got, &gl, SET_TIMEOUT_MS) == 0 &&
			    gl >= 2 && got[0] == v[1] && got[1] == v[0]) match++;
			break;
		case TLV_EXT_PANID:
			if (l != 8) return false;
			have++; gl = sizeof got;
			if (ncp_link_get(SPINEL_PROP_NET_XPANID, got, &gl, SET_TIMEOUT_MS) == 0 &&
			    gl >= 8 && memcmp(got, v, 8) == 0) match++;
			break;
		default: break;
		}
		i += 2 + l;
	}
	return have == 2 && match == 2;
}

int gw_ot_apply_dataset(const uint8_t *tlvs, size_t tlvs_len)
{
	if (!tlvs || tlvs_len == 0) return -EINVAL;

	if (ncp_already_configured(tlvs, tlvs_len)) {
		LOG_INF("NCP already holds this network (PAN-ID/XPAN-ID match) -- keeping it as is, channel included");
		return 0;
	}

	/* NCP rejects PHY/MAC/NET property writes while Thread is up
	 * (last-status=4 invalid-state).  Bring it down first. */
	uint8_t down = 0;
	uint32_t ls = 0;
	(void)ncp_link_set_raw(SPINEL_PROP_NET_STACK_UP, &down, 1, &ls,
			       SET_TIMEOUT_MS);
	(void)ncp_link_set_raw(SPINEL_PROP_NET_IF_UP, &down, 1, &ls,
			       SET_TIMEOUT_MS);
	uint64_t t0 = now_ms();
	int role = wait_detached();
	LOG_INF("NCP brought down (role=%d after %u ms) — applying new dataset",
		role, (unsigned)(now_ms() - t0));

	size_t i = 0;
	int applied = 0;
	int rv;
	while (i + 2 <= tlvs_len) {
		uint8_t t = tlvs[i];
		uint8_t l = tlvs[i + 1];
		if (i + 2 + l > tlvs_len) {
			LOG_WRN("truncated TLV t=%u at offset %zu", t, i);
			return -EINVAL;
		}
		const uint8_t *v = &tlvs[i + 2];
		switch (t) {
		case TLV_CHANNEL:           rv = apply_channel(v, l); break;
		case TLV_PANID:             rv = apply_panid(v, l);   break;
		case TLV_EXT_PANID:         rv = apply_xpanid(v, l);  break;
		case TLV_NETWORK_NAME:      rv = apply_network_name(v, l); break;
		case TLV_NETWORK_KEY:       rv = apply_network_key(v, l);  break;
		case TLV_MESH_LOCAL_PREFIX: rv = apply_mesh_local_prefix(v, l); break;
		case TLV_ACTIVE_TIMESTAMP:
		case TLV_PSKC:
		case TLV_SECURITY_POLICY:
		case TLV_CHANNEL_MASK:
			LOG_DBG("skipping TLV t=%u (defaults)", t);
			rv = 0;
			goto next;
		default:
			LOG_DBG("skipping unknown TLV t=%u len=%u", t, l);
			rv = 0;
			goto next;
		}
		if (rv) return rv;
		applied++;
next:
		i += 2 + l;
	}
	LOG_INF("applied %d TLVs to NCP", applied);
	return 0;
}

int gw_ot_bring_up(void)
{
	uint32_t ls = 0;
	int rv;
	uint8_t up = 1;

	rv = ncp_link_set_raw(SPINEL_PROP_NET_IF_UP, &up, 1, &ls,
			      SET_TIMEOUT_MS);
	if (rv < 0) {
		LOG_ERR("NET_IF_UP rv=%d ls=%u", rv, ls);
		return rv;
	}
	rv = ncp_link_set_raw(SPINEL_PROP_NET_STACK_UP, &up, 1, &ls,
			      SET_TIMEOUT_MS);
	if (rv < 0) {
		LOG_ERR("NET_STACK_UP rv=%d ls=%u", rv, ls);
		return rv;
	}
	LOG_INF("Thread interface UP + stack UP; joining…");
	return 0;
}

int gw_ot_bring_down(void)
{
	uint32_t ls = 0;
	uint8_t down = 0;
	int rv1 = ncp_link_set_raw(SPINEL_PROP_NET_STACK_UP, &down, 1, &ls,
				   SET_TIMEOUT_MS);
	int rv2 = ncp_link_set_raw(SPINEL_PROP_NET_IF_UP, &down, 1, &ls,
				   SET_TIMEOUT_MS);
	if (rv1 < 0 || rv2 < 0) {
		LOG_ERR("bring down: STACK_UP rv=%d IF_UP rv=%d", rv1, rv2);
		return rv1 < 0 ? rv1 : rv2;
	}
	LOG_INF("Thread stack DOWN + interface DOWN");
	return 0;
}
