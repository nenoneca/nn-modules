/* SPDX-License-Identifier: Apache-2.0 */

/*
 * ota_client.c — device-initiated OTA over the nn_proto control plane.
 *
 * Replaces the old CoAP-over-Thread + NAT64 path.  Now uses the same
 * D2H/H2D request/reply pattern as FIELD_OP and the other 0x0020-range
 * control cmds (see docs/protocol/nn_proto.md).
 *
 * Flow:
 *   1. ota_client_check()
 *        D2H OTA_CHECK    { "type":..,"version":.. }
 *      ← H2D OTA_MANIFEST { "update":bool,"version":..,"size":..,
 *                           "sha256":.. }   (or {"update":false})
 *   2. ota_client_download()
 *      For each block N in 0..ceil(size/BLOCK_SIZE)-1:
 *        D2H OTA_BLOCK_REQ  [u32 LE block_num | u16 LE block_size]
 *      ← H2D OTA_BLOCK      [u32 LE block_num | raw bytes]
 *      Bytes are streamed into MCUboot's secondary slot.
 *   3. ota_client_apply() — boot_request_upgrade(TEST) + reboot.
 *
 * Image integrity is guaranteed by MCUboot's signature check on boot.
 */

#include <nn_osal/osal.h>
#include <node_mgr/ota_client.h>
#include <node_mgr/nn_proto_client.h>
#include <nn_proto/nn_proto.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nn_pal/dfu.h>
/* app version via nn_osal_app_version() */
#include <node_mgr/coap_log.h>

#include <psa/crypto.h>   /* per-block sha256 for the chunk-diff path */

NN_OSAL_LOG_MODULE(ota_client);

/* Dedicated heap for OTA buffers.  Caller threads (shell, ota_hint_wq)
 * have small stacks; large CoAP-style buffers must NOT live on the
 * stack.  Sizing covers the simultaneous-allocation worst case during
 * the chunk-diff download:
 *   - sector buffer (4096 B)
 *   - chunksum page (448 B)
 *   - block reply   (~548 B)
 * Plus K_HEAP bookkeeping slack. */
K_HEAP_DEFINE(s_ota_heap, 6144);

#define OTA_HEAP_ALLOC_TIMEOUT_MS  500u

/* Diagnostic: exercise deferred LOG_* from this TU. */
void ota_client_logtest(const char *which)
{
	if (!strcmp(which, "inf")) {
		NN_LOG_INF("logtest INF from ota_client");
	} else if (!strcmp(which, "wrn")) {
		NN_LOG_WRN("logtest WRN from ota_client");
	} else if (!strcmp(which, "err")) {
		NN_LOG_ERR("logtest ERR from ota_client");
	}
}

#define BLOCK_SIZE         512u             /* was 1024: less 6LoWPAN frag */
#define SHA256_LEN         32u
#define MANIFEST_CAP       512u
#define BLOCK_REPLY_CAP    (BLOCK_SIZE + 4u + 32u)   /* hdr + slack */
/* Attempts per request/reply round.  The OTA_CHECK reply (and, for
 * offers, the OTA_MANIFEST carrying patch metadata) is a multi-fragment
 * H2D datagram: losing any 6LoWPAN fragment loses the whole reply, so a
 * weak multi-hop path (observed on c6-s2) needs many attempts before one
 * reply arrives intact.  12 attempts with timeout widening drives the
 * all-attempts-lost probability low enough that distant nodes still
 * converge. */
#define OTA_BLOCK_RETRIES  12

/* Chunk-diff download (see hub proto_router OTA_CHUNKSUMS).  Page size
 * of 56 blocks = exactly 7 flash sectors so sector processing never
 * straddles a page boundary. */
#define SECTOR_SIZE          NN_PAL_DFU_SECTOR_SIZE              /* 4096 */
#define BLOCKS_PER_SECTOR    (SECTOR_SIZE / BLOCK_SIZE)          /* 8 */
#define CHUNKSUM_LEN         8u
#define CHUNKSUM_PAGE_BLOCKS 56u
#define CHUNKSUM_PAGE_CAP    (4u + CHUNKSUM_PAGE_BLOCKS * CHUNKSUM_LEN + 16u)

/* Adaptive request timeout (per round trip).  Thread mesh + 6LoWPAN
 * fragmentation means single-shot fails are common but transient — we
 * widen the deadline on each retry instead of giving up fast. */
#define OTA_TIMEOUT_INIT_MS 2000
#define OTA_TIMEOUT_MAX_MS  10000

/* ── State ────────────────────────────────────────────────────────────────── */

/* hub_addr is no longer used for the OTA path — gateway is auto-resolved
 * by nn_proto_client via the GATEWAY_HELLO multicast.  We keep the
 * setter/getter as a no-op so the shell command surface stays stable. */
static char g_hub_addr[46];
static bool g_auto_apply;

static struct {
	bool     valid;
	char     version[32];
	uint32_t size;
	uint8_t  sha256[SHA256_LEN];
	uint32_t patch_size;   /* >0 when the hub offered a binary delta */
	uint8_t  patch_sha256[SHA256_LEN];
} g_pending;

/* Armed = slot 1 holds a verified image awaiting an operator-confirmed
 * OTA_APPLY (fleet-coordinated reboot).  Persisted to KV so the state
 * survives an unrelated power cycle: on boot, if the running version
 * equals the armed one the apply happened and the flag clears. */
static char g_armed_version[32];

static int g_request_timeout_ms = OTA_TIMEOUT_INIT_MS;

/* ── NVS settings (via nn_osal_kv) ─────────────────────────────────── */

static int ota_kv_load(const char *suffix, const uint8_t *value, size_t len,
		       void *user)
{
	ARG_UNUSED(user);
	if (!strcmp(suffix, "hub_addr") && len > 0 && len < sizeof(g_hub_addr)) {
		memcpy(g_hub_addr, value, len);
		g_hub_addr[len] = '\0';
		return 0;
	}
	if (!strcmp(suffix, "auto_apply") && len == sizeof(bool)) {
		memcpy(&g_auto_apply, value, sizeof(g_auto_apply));
		return 0;
	}
	if (!strcmp(suffix, "armed_ver") && len > 0 &&
	    len < sizeof(g_armed_version)) {
		memcpy(g_armed_version, value, len);
		g_armed_version[len] = '\0';
		return 0;
	}
	return 0;
}

/* ── Hex decode helper ────────────────────────────────────────────────────── */

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
	for (size_t i = 0; i < out_len; i++) {
		unsigned int byte;
		if (sscanf(hex + 2 * i, "%02x", &byte) != 1) {
			return -EINVAL;
		}
		out[i] = (uint8_t)byte;
	}
	return 0;
}

/* Reset and bump the adaptive timeout after a failure. */
static void widen_timeout(void)
{
	int widened = g_request_timeout_ms * 2;
	if (widened > OTA_TIMEOUT_MAX_MS) widened = OTA_TIMEOUT_MAX_MS;
	g_request_timeout_ms = widened;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

/* Base-version compare: Zephyr's APP_VERSION_EXTENDED_STRING carries a
 * `+TWEAK` suffix ("0.0.5+0") while OTA manifests use plain semver. */
static bool base_version_eq(const char *a, const char *b)
{
	while (*a && *b && *a != '+' && *b != '+') {
		if (*a != *b) return false;
		a++; b++;
	}
	return (*a == '\0' || *a == '+') && (*b == '\0' || *b == '+');
}

int ota_client_init(void)
{
	g_pending.valid = false;
	g_request_timeout_ms = OTA_TIMEOUT_INIT_MS;
	(void)nn_osal_kv_register("ota", ota_kv_load, NULL);

	/* Armed-state reconciliation: if we booted into the version we
	 * were armed for, the operator apply happened — clear the flag.
	 * If we booted into something else while armed, slot 1 may have
	 * been invalidated by the swap machinery; clear too and let the
	 * next check/download re-arm. */
	if (g_armed_version[0]) {
		if (base_version_eq(g_armed_version, nn_osal_app_version())) {
			NN_LOG_INF("OTA: armed apply for %s completed",
				g_armed_version);
		} else {
			NN_LOG_WRN("OTA: armed for %s but running %s — clearing",
				g_armed_version, nn_osal_app_version());
		}
		g_armed_version[0] = '\0';
		nn_osal_kv_delete("ota/armed_ver");
	}

	NN_LOG_INF("OTA client init: nn_proto transport, auto_apply=%d",
		g_auto_apply);
	return 0;
}

const char *ota_client_armed_version(void)
{
	return g_armed_version;
}

int ota_client_arm(void)
{
	if (!g_pending.valid) {
		return -EINVAL;
	}
	strncpy(g_armed_version, g_pending.version,
		sizeof(g_armed_version) - 1);
	g_armed_version[sizeof(g_armed_version) - 1] = '\0';
	nn_osal_kv_save("ota/armed_ver", g_armed_version,
			strlen(g_armed_version) + 1);

	/* Best-effort OTA_READY (fire-and-forget; the hub's INFO_QUERY
	 * watchdog + the armed field in INFO_REPLY are the reliable
	 * backstop). */
	char sha_hex[2 * SHA256_LEN + 1];
	for (size_t i = 0; i < SHA256_LEN; i++) {
		snprintf(sha_hex + 2 * i, 3, "%02x", g_pending.sha256[i]);
	}
	char json[128];
	int n = snprintf(json, sizeof(json),
			 "{\"version\":\"%s\",\"sha256\":\"%s\"}",
			 g_armed_version, sha_hex);
	if (n > 0 && n < (int)sizeof(json)) {
		(void)nn_proto_client_send_d2h_reply(NN_PROTO_CMD_OTA_READY, 0,
						     (const uint8_t *)json,
						     (size_t)n);
	}
	printk("OTA: armed for %s — awaiting operator apply\n",
	       g_armed_version);
	return 0;
}

int ota_client_set_hub_addr(const char *addr)
{
	if (strlen(addr) >= sizeof(g_hub_addr)) {
		return -EINVAL;
	}
	strncpy(g_hub_addr, addr, sizeof(g_hub_addr) - 1);
	g_hub_addr[sizeof(g_hub_addr) - 1] = '\0';
	nn_osal_kv_save("ota/hub_addr", g_hub_addr, strlen(g_hub_addr) + 1);
	NN_LOG_INF("Hub address noted (unused — gateway auto-discovered): %s",
		g_hub_addr);
	return 0;
}

const char *ota_client_get_hub_addr(void)
{
	return g_hub_addr;
}

void ota_client_set_auto_apply(bool enable)
{
	g_auto_apply = enable;
	nn_osal_kv_save("ota/auto_apply", &g_auto_apply, sizeof(g_auto_apply));
	NN_LOG_INF("Auto-apply: %s", enable ? "on" : "off");
}

bool ota_client_get_auto_apply(void)
{
	return g_auto_apply;
}

const char *ota_client_pending_version(void)
{
	return g_pending.valid ? g_pending.version : "";
}

uint32_t ota_client_pending_size(void)
{
	return g_pending.valid ? g_pending.size : 0;
}

/* ── Check for update ─────────────────────────────────────────────────────── */

int ota_client_check(void)
{
	char req_json[128];
	int rlen = snprintf(req_json, sizeof(req_json),
			    "{\"type\":\"%s\",\"version\":\"%s\"}",
			    CONFIG_APP_DEVICE_TYPE,
			    nn_osal_app_version());
	if (rlen < 0 || rlen >= (int)sizeof(req_json)) {
		return -EINVAL;
	}

	uint8_t *resp = k_heap_alloc(&s_ota_heap, MANIFEST_CAP,
				     K_MSEC(OTA_HEAP_ALLOC_TIMEOUT_MS));
	if (!resp) {
		NN_LOG_ERR("OTA check: heap alloc failed (%u B)", MANIFEST_CAP);
		return -ENOMEM;
	}
	int ret = -ETIMEDOUT;
	int last = -ETIMEDOUT;

	for (int retry = 0; retry <= OTA_BLOCK_RETRIES; retry++) {
		if (retry > 0) {
			widen_timeout();
			printk("ota: check retry %d (timeout=%d ms)\n",
			       retry, g_request_timeout_ms);
			nn_osal_sleep_ms(50);
		}
		size_t resp_len = MANIFEST_CAP - 1;
		int rv = nn_proto_client_request_d2h(
			NN_PROTO_CMD_OTA_CHECK,
			(const uint8_t *)req_json, (size_t)rlen,
			NN_PROTO_CMD_OTA_MANIFEST,
			resp, &resp_len, g_request_timeout_ms);
		if (rv == 0) {
			resp[resp_len] = '\0';

			if (!strstr((char *)resp, "\"update\": true") &&
			    !strstr((char *)resp, "\"update\":true")) {
				NN_LOG_INF("OTA: up to date (%s)",
					nn_osal_app_version());
				g_pending.valid = false;
				ret = 0;
				goto out;
			}

			const char *vp = strstr((char *)resp, "\"version\":");
			if (vp) {
				vp = strchr(vp, ':') + 1;
				while (*vp == ' ' || *vp == '"') vp++;
				char *ve = g_pending.version;
				while (*vp && *vp != '"' &&
				       (ve - g_pending.version) < 31) {
					*ve++ = *vp++;
				}
				*ve = '\0';
			}
			const char *sp = strstr((char *)resp, "\"size\":");
			if (sp) {
				g_pending.size = (uint32_t)atoi(sp + 7);
			}
			const char *hp = strstr((char *)resp, "\"sha256\":");
			if (hp) {
				hp = strchr(hp + 9, '"');
				if (hp) {
					hp++;
					hex_to_bytes(hp, g_pending.sha256,
						     SHA256_LEN);
				}
			}
			/* Optional patch offer: "patch":{"size":N,...}.  The
			 * outer "size" (full image) is parsed above; the patch
			 * size lives inside the nested object, so search from
			 * the "patch" key. */
			g_pending.patch_size = 0;
#ifdef CONFIG_NODE_MGR_OTA_DELTA
			const char *pp = strstr((char *)resp, "\"patch\":");
			if (pp) {
				const char *ps = strstr(pp, "\"size\":");
				if (ps) {
					g_pending.patch_size =
						(uint32_t)atoi(ps + 7);
				}
				/* patch sha256 lives inside the patch object */
				const char *psh = strstr(pp, "\"sha256\":");
				if (psh) {
					psh = strchr(psh + 9, '"');
					if (psh) {
						hex_to_bytes(psh + 1,
							g_pending.patch_sha256,
							SHA256_LEN);
					}
				}
			}
#endif
			g_pending.valid = true;
			if (g_pending.patch_size) {
				NN_LOG_INF("OTA: update %s (%u B, patch %u B)",
					g_pending.version, g_pending.size,
					g_pending.patch_size);
			} else {
				NN_LOG_INF("OTA: update available %s (%u B)",
					g_pending.version, g_pending.size);
			}
			coap_log_send("OTA update: %s %u B",
				      g_pending.version, g_pending.size);
			ret = 1;
			goto out;
		}
		last = rv;
		printk("ota: check rv=%d\n", rv);
	}

	NN_LOG_ERR("OTA check failed after retries: %d", last);
	ret = last;

out:
	k_heap_free(&s_ota_heap, resp);
	return ret;
}

/* ── Download firmware via OTA_BLOCK_REQ ──────────────────────────────────── */

/* DFU context for the active OTA — the PAL backend allocates static
 * storage internally so this pointer is just a handle. */
static nn_pal_dfu_ctx_t *g_dfu_ctx;

/* Fetch one image block from the hub with the adaptive retry policy.
 * On success, *reply holds [u32 LE block_num | payload] and *reply_len
 * the total reply length.  Shared by the chunk-diff path. */
static int fetch_block(uint32_t block_num, uint16_t want,
		       uint8_t *reply, size_t *reply_len)
{
	uint8_t req[6];
	req[0] = (uint8_t)(block_num);
	req[1] = (uint8_t)(block_num >> 8);
	req[2] = (uint8_t)(block_num >> 16);
	req[3] = (uint8_t)(block_num >> 24);
	req[4] = (uint8_t)(want);
	req[5] = (uint8_t)(want >> 8);

	int rv = -ETIMEDOUT;
	for (int retry = 0; retry <= OTA_BLOCK_RETRIES; retry++) {
		if (retry > 0) {
			widen_timeout();
			printk("OTA: block %u retry %d (timeout=%d ms)\n",
			       block_num, retry, g_request_timeout_ms);
			nn_osal_sleep_ms(50);
		}
		*reply_len = BLOCK_REPLY_CAP;
		rv = nn_proto_client_request_d2h(
			NN_PROTO_CMD_OTA_BLOCK_REQ,
			req, sizeof(req),
			NN_PROTO_CMD_OTA_BLOCK,
			reply, reply_len, g_request_timeout_ms);
		if (rv == 0 && *reply_len >= 4) {
			uint32_t got_num =
				((uint32_t)reply[0]) |
				((uint32_t)reply[1] << 8) |
				((uint32_t)reply[2] << 16) |
				((uint32_t)reply[3] << 24);
			if (got_num == block_num) {
				return 0;
			}
			printk("OTA: block %u: hub sent %u (mismatch)\n",
			       block_num, got_num);
			rv = -EBADMSG;
		}
	}
	printk("OTA: block %u failed after %d retries: %d\n",
	       block_num, OTA_BLOCK_RETRIES, rv);
	return rv;
}

/* ── Chunk-diff download ─────────────────────────────────────────────────
 *
 * Instead of pre-erasing slot 1 and streaming every block from the hub,
 * fetch the target image's per-block checksum table and process the
 * image sector by sector (4 KiB = 8 blocks):
 *
 *   1. slot 1 sector already matches the target → keep as-is (no erase,
 *      no write).  Covers resume-after-interrupt and re-push.
 *   2. otherwise assemble the sector: blocks whose checksum matches the
 *      RUNNING image (slot 0) are copied locally; the rest are
 *      downloaded.  Adjacent firmware versions are largely identical,
 *      so for version bumps nearly everything copies locally.
 *   3. erase + write the assembled sector.
 *
 * Ends with a whole-image sha256 verification against the manifest —
 * a checksum-logic bug can cost a retry, never a bad image (and
 * MCUboot's signature check at swap remains the final gate).
 */

/* PSA hash serialization: shares the identity lock used by all other
 * PSA callers in this firmware (see nn_proto_identity.h). */
#include <nn_proto_identity/nn_proto_identity.h>

#ifdef CONFIG_NODE_MGR_OTA_DELTA
#include <detools.h>

/* ── Binary-delta (detools) download ─────────────────────────────────────
 *
 * When the hub's OTA_MANIFEST carries a "patch" offer, reconstruct the
 * target image in slot 1 from slot 0 (the running image) + a streamed
 * detools patch.  Typical code-change update: ~30 KB patch vs ~1 MB
 * full image.
 *
 * detools drives three callbacks as it consumes the patch:
 *   from_read/from_seek → random-access read of slot 0
 *   to_write            → sequential write into slot 1
 * We feed the patch bytes in via detools_apply_patch_process() as they
 * arrive from OTA_PATCH; the lib pulls slot-0 bytes + emits slot-1
 * bytes on its own schedule.
 */

struct dd_ctx {
	uint32_t from_pos;     /* slot 0 read cursor */
	uint32_t to_pos;       /* slot 1 write cursor (sequential) */
	uint32_t to_size;      /* target image size (for sector erase) */
	uint32_t erased_upto;  /* slot 1 erased up to this offset */
	uint8_t  sect[SECTOR_SIZE];
	uint32_t sect_base;    /* offset of the buffered output sector */
	uint32_t sect_len;     /* valid bytes in sect */
};

static int dd_from_read(void *arg, uint8_t *buf, size_t size)
{
	struct dd_ctx *c = arg;
	int rc = nn_pal_dfu_slot_read(NN_PAL_DFU_SLOT_PRIMARY,
				      c->from_pos, buf, size);
	if (rc) return -DETOOLS_IO_FAILED;
	c->from_pos += size;
	return 0;
}

static int dd_from_seek(void *arg, int offset)
{
	struct dd_ctx *c = arg;
	c->from_pos = (uint32_t)((int)c->from_pos + offset);
	return 0;
}

/* Flush the buffered output sector to slot 1 (erase + write, 4B pad). */
static int dd_flush_sector(struct dd_ctx *c)
{
	if (c->sect_len == 0) return 0;
	int rc = nn_pal_dfu_slot1_erase(c->sect_base, SECTOR_SIZE);
	if (rc) return rc;
	size_t wlen = (c->sect_len + 3) & ~(size_t)3;
	if (wlen > c->sect_len) {
		memset(c->sect + c->sect_len, 0xFF, wlen - c->sect_len);
	}
	rc = nn_pal_dfu_slot1_write(c->sect_base, c->sect, wlen);
	if (rc) return rc;
	c->sect_len = 0;
	return 0;
}

static int dd_to_write(void *arg, const uint8_t *buf, size_t size)
{
	struct dd_ctx *c = arg;
	while (size > 0) {
		uint32_t sect_idx = c->to_pos / SECTOR_SIZE;
		uint32_t base = sect_idx * SECTOR_SIZE;
		if (c->sect_len == 0) {
			c->sect_base = base;
		} else if (base != c->sect_base) {
			int rc = dd_flush_sector(c);
			if (rc) return -DETOOLS_IO_FAILED;
			c->sect_base = base;
		}
		uint32_t within = c->to_pos - base;
		size_t n = SECTOR_SIZE - within;
		if (n > size) n = size;
		memcpy(c->sect + within, buf, n);
		if (within + n > c->sect_len) c->sect_len = within + n;
		c->to_pos += n;
		buf += n;
		size -= n;
		if (c->sect_len == SECTOR_SIZE) {
			int rc = dd_flush_sector(c);
			if (rc) return -DETOOLS_IO_FAILED;
		}
	}
	return 0;
}

/* Fetch one patch chunk with the adaptive retry policy.  Reply is
 * [u32 LE offset | patch bytes]. */
static int fetch_patch(uint32_t offset, uint16_t want,
		       uint8_t *reply, size_t *reply_len)
{
	uint8_t req[6];
	req[0] = (uint8_t)(offset);
	req[1] = (uint8_t)(offset >> 8);
	req[2] = (uint8_t)(offset >> 16);
	req[3] = (uint8_t)(offset >> 24);
	req[4] = (uint8_t)(want);
	req[5] = (uint8_t)(want >> 8);

	int rv = -ETIMEDOUT;
	for (int retry = 0; retry <= OTA_BLOCK_RETRIES; retry++) {
		if (retry > 0) {
			widen_timeout();
			nn_osal_sleep_ms(50);
		}
		*reply_len = BLOCK_REPLY_CAP;
		rv = nn_proto_client_request_d2h(
			NN_PROTO_CMD_OTA_PATCH_REQ, req, sizeof(req),
			NN_PROTO_CMD_OTA_PATCH, reply, reply_len,
			g_request_timeout_ms);
		if (rv == 0 && *reply_len >= 4) {
			uint32_t got =
				((uint32_t)reply[0]) | ((uint32_t)reply[1] << 8) |
				((uint32_t)reply[2] << 16) | ((uint32_t)reply[3] << 24);
			if (got == offset) return 0;
			rv = -EBADMSG;
		}
	}
	printk("OTA: patch off %u failed: %d\n", offset, rv);
	return rv;
}

/* Where the patch is staged within slot 1.  Set by delta_download before
 * the apply phase; dd_patch_read serves the apply loop from here. */
static uint32_t g_patch_stage_off;

/* Compute a sha256 over a slot-1 byte range, into out[32]. */
static int slot1_sha256(uint32_t off, uint32_t len, uint8_t *scratch,
			uint8_t out[32])
{
	psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
	size_t dlen = 0;
	int rc = 0;
	nn_proto_identity_psa_lock();
	psa_status_t ps = psa_hash_setup(&op, PSA_ALG_SHA_256);
	for (uint32_t o = 0; ps == PSA_SUCCESS && o < len; o += SECTOR_SIZE) {
		size_t n = (len - o) < SECTOR_SIZE ? (len - o) : SECTOR_SIZE;
		if (nn_pal_dfu_slot_read(NN_PAL_DFU_SLOT_SECONDARY,
					 off + o, scratch, n)) { rc = -EIO; break; }
		ps = psa_hash_update(&op, scratch, n);
	}
	if (ps == PSA_SUCCESS && rc == 0)
		ps = psa_hash_finish(&op, out, 32, &dlen);
	else
		psa_hash_abort(&op);
	nn_proto_identity_psa_unlock();
	if (rc) return rc;
	return (ps == PSA_SUCCESS) ? 0 : -EBADMSG;
}

/* Defined later (shared with the chunk-diff path); forward-declared so the
 * delta stager can reuse the per-block checksum primitives. */
static void block_sum(const uint8_t *p, size_t len, uint8_t out[CHUNKSUM_LEN]);
static int  fetch_chunksums(uint32_t first_block, uint16_t count,
			    uint8_t target, uint8_t *sums, uint16_t *got);

/* Stall watchdog for the resumable patch stager: if no forward progress (a
 * block downloaded or a sector skipped) happens within this window, abort
 * cleanly so a wedged mesh path can never hang the OTA worker — the staged
 * sectors survive in flash and a re-hint resumes from where it left off. */
#define OTA_STAGE_STALL_MS  90000u

/* Phase 1 of the delta path, made robust + RESUMABLE (chunked, checksummed):
 * download the detools patch into the slot-1 tail in 4 KiB sectors, each
 * verified block-by-block against the hub's per-block patch chunksum table
 * (fetch_chunksums target=1).  A sector whose already-staged bytes match is
 * SKIPPED (resume); only mismatched blocks are (re)fetched via OTA_PATCH; a
 * stall watchdog converts a stuck mesh into a clean abort.  Mirrors the
 * chunk-diff image path but addresses the patch artifact and stages in the
 * tail.  Returns 0, or -ETIMEDOUT/-EIO (transient → resume on re-hint), or
 * other negative (structural → caller may fall back to chunk-diff). */
static int delta_stage_patch(struct dd_ctx *c, uint8_t *reply, uint8_t *sums)
{
	const uint32_t psize    = g_pending.patch_size;
	const uint32_t n_blocks = (psize + BLOCK_SIZE - 1) / BLOCK_SIZE;
	uint32_t page_first   = UINT32_MAX;
	uint16_t page_count   = 0;
	uint32_t last_prog_ms = nn_osal_uptime_ms_32();
	uint32_t st_dl = 0, st_skip = 0;

	for (uint32_t soff = 0; soff < psize; soff += SECTOR_SIZE) {
		uint32_t sect_len = (psize - soff) < SECTOR_SIZE
				    ? (psize - soff) : SECTOR_SIZE;
		uint32_t nblk = (sect_len + BLOCK_SIZE - 1) / BLOCK_SIZE;
		uint32_t first_block = soff / BLOCK_SIZE;
		uint32_t stage_addr  = g_patch_stage_off + soff;

		if (nn_osal_uptime_ms_32() - last_prog_ms > OTA_STAGE_STALL_MS) {
			printk("OTA: patch stage stalled — abort (resume on re-hint)\n");
			return -ETIMEDOUT;
		}

		/* Page in the patch chunksums covering this sector's blocks. */
		if (page_first == UINT32_MAX || first_block < page_first ||
		    first_block + nblk > page_first + page_count) {
			uint16_t want = (n_blocks - first_block) < CHUNKSUM_PAGE_BLOCKS
					? (uint16_t)(n_blocks - first_block)
					: (uint16_t)CHUNKSUM_PAGE_BLOCKS;
			int rv = fetch_chunksums(first_block, want, 1 /* patch */,
						 sums, &page_count);
			if (rv) { printk("OTA: patch chunksums %d\n", rv); return rv; }
			page_first = first_block;
		}

		/* 1) Resume: skip the sector if its staged bytes already match. */
		bool have = (nn_pal_dfu_slot_read(NN_PAL_DFU_SLOT_SECONDARY,
						  stage_addr, c->sect, sect_len) == 0);
		for (uint32_t b = 0; have && b < nblk; b++) {
			uint32_t blen = (sect_len - b * BLOCK_SIZE) < BLOCK_SIZE
					? (sect_len - b * BLOCK_SIZE) : BLOCK_SIZE;
			uint8_t dg[CHUNKSUM_LEN];
			block_sum(c->sect + b * BLOCK_SIZE, blen, dg);
			if (memcmp(dg, sums + (first_block - page_first + b) * CHUNKSUM_LEN,
				   CHUNKSUM_LEN) != 0) { have = false; }
		}
		if (have) { st_skip++; last_prog_ms = nn_osal_uptime_ms_32(); continue; }

		/* 2) (Re)fetch the sector block-by-block, verifying each block. */
		int rc = nn_pal_dfu_slot1_erase(stage_addr, SECTOR_SIZE);
		if (rc) { printk("OTA: stage erase %d\n", rc); return rc; }
		for (uint32_t b = 0; b < nblk; b++) {
			uint32_t boff = soff + b * BLOCK_SIZE;
			uint32_t blen = (sect_len - b * BLOCK_SIZE) < BLOCK_SIZE
					? (sect_len - b * BLOCK_SIZE) : BLOCK_SIZE;
			const uint8_t *want_sum =
				sums + (first_block - page_first + b) * CHUNKSUM_LEN;
			bool ok = false;
			/* fetch_patch already retries transport internally; the
			 * small outer loop re-fetches on the rare checksum
			 * mismatch (corruption that survived the offset echo). */
			for (int outer = 0; outer < 3 && !ok; outer++) {
				size_t rl = 0;
				if (fetch_patch(boff, (uint16_t)blen, reply, &rl) == 0 &&
				    rl >= 4u + blen) {
					uint8_t dg[CHUNKSUM_LEN];
					block_sum(reply + 4, blen, dg);
					if (memcmp(dg, want_sum, CHUNKSUM_LEN) == 0) {
						memcpy(c->sect + b * BLOCK_SIZE,
						       reply + 4, blen);
						ok = true;
						last_prog_ms = nn_osal_uptime_ms_32();
					}
				}
			}
			if (!ok) {
				printk("OTA: patch block @%u unrecoverable\n", boff);
				return -EIO;
			}
		}
		uint32_t wlen = (sect_len + 3u) & ~3u;
		if (wlen > sect_len) memset(c->sect + sect_len, 0xFF, wlen - sect_len);
		rc = nn_pal_dfu_slot1_write(stage_addr, c->sect, wlen);
		if (rc) { printk("OTA: stage write %d\n", rc); return rc; }
		st_dl++;
		printk("OTA: patch sect %u/%u staged\n",
		       (soff / SECTOR_SIZE) + 1u,
		       (psize + SECTOR_SIZE - 1u) / SECTOR_SIZE);
	}
	printk("OTA: patch staged (%u dl, %u resumed)\n", st_dl, st_skip);
	return 0;
}

/* Proposal B: decouple patch DOWNLOAD from APPLY.
 *
 * (1) download the ENTIRE patch into the tail of slot 1 — now a CHUNKED,
 * checksummed, RESUMABLE stager (delta_stage_patch) that can re-fetch only
 * the bad/missing sectors and survive a mesh stall; (2) verify the complete
 * patch sha256; (3) apply locally, reading the patch back from flash so the
 * apply never touches the mesh and cannot stall; (4) verify the
 * reconstructed whole image.  The reconstructed image is written to the
 * START of slot 1 (sector-buffered); it never reaches the staged patch in
 * the tail (asserted by the g_pending.size > g_patch_stage_off guard).
 */
static int delta_download(void)
{
	struct dd_ctx *c = k_heap_alloc(&s_ota_heap, sizeof(*c),
					K_MSEC(OTA_HEAP_ALLOC_TIMEOUT_MS));
	uint8_t *reply = k_heap_alloc(&s_ota_heap, BLOCK_REPLY_CAP,
				      K_MSEC(OTA_HEAP_ALLOC_TIMEOUT_MS));
	uint8_t *sums = k_heap_alloc(&s_ota_heap,
				     CHUNKSUM_PAGE_BLOCKS * CHUNKSUM_LEN,
				     K_MSEC(OTA_HEAP_ALLOC_TIMEOUT_MS));
	int rc = -ENOMEM;
	if (!c || !reply || !sums) { printk("OTA: delta heap alloc failed\n"); goto out; }

	uint32_t slot1_sz = nn_pal_dfu_slot1_size();
	uint32_t patch_region = (g_pending.patch_size + SECTOR_SIZE - 1)
				& ~(SECTOR_SIZE - 1);
	if (slot1_sz == 0 || patch_region == 0 ||
	    patch_region >= slot1_sz) {
		printk("OTA: delta bad sizes slot1=%u patch=%u\n",
		       slot1_sz, g_pending.patch_size);
		rc = -EINVAL; goto out;
	}
	g_patch_stage_off = slot1_sz - patch_region;
	if (g_pending.size > g_patch_stage_off) {
		printk("OTA: image %u would overrun patch stage @%u\n",
		       g_pending.size, g_patch_stage_off);
		rc = -ENOSPC; goto out;   /* fall back to chunk-diff/full */
	}

	uint32_t t0 = nn_osal_uptime_ms_32();

	/* ── Phase 1: stage the whole patch into slot 1 tail ─────────────
	 * Chunked + per-block-checksummed + RESUMABLE.  No blanket pre-erase
	 * (the caller no longer wipes slot 1 either) so an interrupted run's
	 * already-staged sectors are kept and skipped on the next attempt. */
	memset(c, 0, sizeof(*c));            /* reuse c->sect as stage buffer */
	c->sect_base = 0;
	rc = delta_stage_patch(c, reply, sums);
	if (rc) goto out;     /* -ETIMEDOUT/-EIO: resume; other: caller falls back */

	/* ── Phase 2: verify staged patch sha256 ──────────────────────── */
	{
		uint8_t dg[32];
		rc = slot1_sha256(g_patch_stage_off, g_pending.patch_size,
				  c->sect, dg);
		if (rc) goto out;
		if (memcmp(dg, g_pending.patch_sha256, SHA256_LEN) != 0) {
			printk("OTA: staged patch sha MISMATCH\n");
			rc = -EBADMSG; goto out;
		}
	}
	printk("OTA: patch staged + verified (%u B) — applying locally\n",
	       g_pending.patch_size);

	/* ── Phase 3: apply (all local — no mesh) ─────────────────────── */
	memset(c, 0, sizeof(*c));
	c->to_size = g_pending.size;
	struct detools_apply_patch_t apply;
	int dr = detools_apply_patch_init(&apply, dd_from_read, dd_from_seek,
					  g_pending.patch_size, dd_to_write, c);
	if (dr != 0) { printk("OTA: detools init %d\n", dr); rc = -EIO; goto out; }

	for (uint32_t po = 0; po < g_pending.patch_size; ) {
		uint32_t remain = g_pending.patch_size - po;
		size_t n = remain < BLOCK_SIZE ? remain : BLOCK_SIZE;
		rc = nn_pal_dfu_slot_read(NN_PAL_DFU_SLOT_SECONDARY,
					  g_patch_stage_off + po, reply, n);
		if (rc) { printk("OTA: patch readback %d\n", rc); goto out; }
		dr = detools_apply_patch_process(&apply, reply, n);
		if (dr < 0) { printk("OTA: detools process %d\n", dr);
			rc = -EIO; goto out; }
		po += n;
	}
	dr = detools_apply_patch_finalize(&apply);   /* >0 = to-size, <0 = err */
	if (dr < 0) { printk("OTA: detools finalize %d\n", dr); rc = -EIO; goto out; }
	if ((uint32_t)dr != g_pending.size) {
		printk("OTA: detools to-size %d != %u\n", dr, g_pending.size);
		rc = -EBADMSG; goto out;
	}
	rc = dd_flush_sector(c);
	if (rc) { printk("OTA: delta final flush %d\n", rc); goto out; }
	if (c->to_pos != g_pending.size) {
		printk("OTA: delta size mismatch %u != %u\n",
		       c->to_pos, g_pending.size);
		rc = -EBADMSG; goto out;
	}

	/* ── Phase 4: whole-image sha256 verify ───────────────────────── */
	{
		uint8_t dg[32];
		rc = slot1_sha256(0, g_pending.size, c->sect, dg);
		if (rc) goto out;
		if (memcmp(dg, g_pending.sha256, SHA256_LEN) != 0) {
			printk("OTA: delta image verify FAILED\n");
			rc = -EBADMSG; goto out;
		}
	}
	printk("OTA delta complete in %u ms: %u B patch → %u B image\n",
	       (unsigned)(nn_osal_uptime_ms_32() - t0),
	       g_pending.patch_size, g_pending.size);
	rc = 0;
out:
	if (c)     k_heap_free(&s_ota_heap, c);
	if (reply) k_heap_free(&s_ota_heap, reply);
	if (sums)  k_heap_free(&s_ota_heap, sums);
	return rc;
}
#endif /* CONFIG_NODE_MGR_OTA_DELTA */

static void block_sum(const uint8_t *p, size_t len,
		      uint8_t out[CHUNKSUM_LEN])
{
	uint8_t full[32];
	size_t  olen = 0;
	nn_proto_identity_psa_lock();
	(void)psa_hash_compute(PSA_ALG_SHA_256, p, len,
			       full, sizeof full, &olen);
	nn_proto_identity_psa_unlock();
	memcpy(out, full, CHUNKSUM_LEN);
}

/* Fetch a page of per-block checksums.  `target` selects which artifact the
 * block indices address: 0 = the target image (chunk-diff), 1 = the staged
 * detools patch (delta resume).  Returns 0 and sets *got to the number of
 * block sums written to `sums`, or negative errno. */
static int fetch_chunksums(uint32_t first_block, uint16_t count,
			   uint8_t target, uint8_t *sums, uint16_t *got)
{
	uint8_t req[7];
	req[0] = (uint8_t)(first_block);
	req[1] = (uint8_t)(first_block >> 8);
	req[2] = (uint8_t)(first_block >> 16);
	req[3] = (uint8_t)(first_block >> 24);
	req[4] = (uint8_t)(count);
	req[5] = (uint8_t)(count >> 8);
	req[6] = target;

	uint8_t  reply[CHUNKSUM_PAGE_CAP];
	int rv = -ETIMEDOUT;
	for (int retry = 0; retry < 3; retry++) {
		if (retry > 0) {
			widen_timeout();
			nn_osal_sleep_ms(50);
		}
		size_t reply_len = sizeof(reply);
		rv = nn_proto_client_request_d2h(
			NN_PROTO_CMD_OTA_CHUNKSUMS_REQ,
			req, sizeof(req),
			NN_PROTO_CMD_OTA_CHUNKSUMS,
			reply, &reply_len, g_request_timeout_ms);
		if (rv == 0 && reply_len >= 4 + CHUNKSUM_LEN) {
			uint32_t echo =
				((uint32_t)reply[0]) |
				((uint32_t)reply[1] << 8) |
				((uint32_t)reply[2] << 16) |
				((uint32_t)reply[3] << 24);
			if (echo != first_block) {
				rv = -EBADMSG;
				continue;
			}
			uint16_t n = (uint16_t)((reply_len - 4) / CHUNKSUM_LEN);
			memcpy(sums, reply + 4, (size_t)n * CHUNKSUM_LEN);
			*got = n;
			return 0;
		}
	}
	return rv;
}

static int chunkdiff_download(void)
{
	const uint32_t total_blocks =
		(g_pending.size + BLOCK_SIZE - 1) / BLOCK_SIZE;

	uint8_t *sect = k_heap_alloc(&s_ota_heap, SECTOR_SIZE,
				     K_MSEC(OTA_HEAP_ALLOC_TIMEOUT_MS));
	uint8_t *sums = k_heap_alloc(&s_ota_heap,
				     CHUNKSUM_PAGE_BLOCKS * CHUNKSUM_LEN,
				     K_MSEC(OTA_HEAP_ALLOC_TIMEOUT_MS));
	uint8_t *reply = k_heap_alloc(&s_ota_heap, BLOCK_REPLY_CAP,
				      K_MSEC(OTA_HEAP_ALLOC_TIMEOUT_MS));
	int rc = -ENOMEM;
	if (!sect || !sums || !reply) {
		printk("OTA: chunkdiff heap alloc failed\n");
		goto out;
	}

	uint32_t page_first = UINT32_MAX;
	uint16_t page_count = 0;
	uint32_t st_skip = 0, st_local = 0, st_dl = 0;
	uint32_t t0 = nn_osal_uptime_ms_32();

	for (uint32_t sect_idx = 0;
	     sect_idx * SECTOR_SIZE < g_pending.size; sect_idx++) {
		const uint32_t sect_off  = sect_idx * SECTOR_SIZE;
		const uint32_t first_blk = sect_idx * BLOCKS_PER_SECTOR;
		const uint32_t blocks_here =
			(total_blocks - first_blk) < BLOCKS_PER_SECTOR
			? (total_blocks - first_blk) : BLOCKS_PER_SECTOR;
		const size_t sect_len =
			(g_pending.size - sect_off) < SECTOR_SIZE
			? (g_pending.size - sect_off) : SECTOR_SIZE;

		/* Page in the target checksums for this sector. */
		if (page_first == UINT32_MAX ||
		    first_blk + blocks_here > page_first + page_count) {
			rc = fetch_chunksums(first_blk, CHUNKSUM_PAGE_BLOCKS,
					     0 /* image */, sums, &page_count);
			if (rc) {
				printk("OTA: chunksums @%u rv=%d\n",
				       first_blk, rc);
				goto out;
			}
			page_first = first_blk;
		}
		const uint8_t *tsums =
			sums + (size_t)(first_blk - page_first) * CHUNKSUM_LEN;

		/* 1) Does slot 1 already hold this sector? */
		rc = nn_pal_dfu_slot_read(NN_PAL_DFU_SLOT_SECONDARY,
					  sect_off, sect, sect_len);
		if (rc) goto out;
		bool b_match = true;
		for (uint32_t i = 0; i < blocks_here; i++) {
			size_t bl = (sect_len - i * BLOCK_SIZE) < BLOCK_SIZE
				? (sect_len - i * BLOCK_SIZE) : BLOCK_SIZE;
			uint8_t cur[CHUNKSUM_LEN];
			block_sum(sect + i * BLOCK_SIZE, bl, cur);
			if (memcmp(cur, tsums + i * CHUNKSUM_LEN,
				   CHUNKSUM_LEN) != 0) {
				b_match = false;
				break;
			}
		}
		if (b_match) {
			st_skip += blocks_here;
			continue;
		}

		/* 2) Assemble: local copy from slot 0 where possible,
		 * download the rest. */
		rc = nn_pal_dfu_slot_read(NN_PAL_DFU_SLOT_PRIMARY,
					  sect_off, sect, sect_len);
		if (rc) goto out;
		for (uint32_t i = 0; i < blocks_here; i++) {
			size_t bl = (sect_len - i * BLOCK_SIZE) < BLOCK_SIZE
				? (sect_len - i * BLOCK_SIZE) : BLOCK_SIZE;
			uint8_t cur[CHUNKSUM_LEN];
			block_sum(sect + i * BLOCK_SIZE, bl, cur);
			if (memcmp(cur, tsums + i * CHUNKSUM_LEN,
				   CHUNKSUM_LEN) == 0) {
				st_local++;
				continue;
			}
			size_t reply_len = 0;
			rc = fetch_block(first_blk + i, (uint16_t)bl,
					 reply, &reply_len);
			if (rc) goto out;
			if (reply_len - 4 != bl) {
				printk("OTA: block %u short payload %u\n",
				       first_blk + i,
				       (unsigned)(reply_len - 4));
				rc = -EBADMSG;
				goto out;
			}
			memcpy(sect + i * BLOCK_SIZE, reply + 4, bl);
			st_dl++;
		}

		/* 3) Erase + write the assembled sector.  Pad the write
		 * to 4-byte alignment with erased-flash bytes. */
		rc = nn_pal_dfu_slot1_erase(sect_off, SECTOR_SIZE);
		if (rc) goto out;
		size_t wlen = (sect_len + 3) & ~(size_t)3;
		if (wlen > sect_len) {
			memset(sect + sect_len, 0xFF, wlen - sect_len);
		}
		rc = nn_pal_dfu_slot1_write(sect_off, sect, wlen);
		if (rc) goto out;

		if ((sect_idx & 31) == 0) {
			printk("OTA: chunkdiff sector %u/%u (skip=%u local=%u dl=%u)\n",
			       sect_idx,
			       (unsigned)((g_pending.size + SECTOR_SIZE - 1)
					  / SECTOR_SIZE),
			       st_skip, st_local, st_dl);
		}
	}

	/* Whole-image verification against the manifest sha256. */
	{
		psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
		uint8_t digest[32];
		size_t  dlen = 0;
		nn_proto_identity_psa_lock();
		psa_status_t ps = psa_hash_setup(&op, PSA_ALG_SHA_256);
		for (uint32_t off = 0;
		     ps == PSA_SUCCESS && off < g_pending.size;
		     off += SECTOR_SIZE) {
			size_t n = (g_pending.size - off) < SECTOR_SIZE
				? (g_pending.size - off) : SECTOR_SIZE;
			rc = nn_pal_dfu_slot_read(NN_PAL_DFU_SLOT_SECONDARY,
						  off, sect, n);
			if (rc) break;
			ps = psa_hash_update(&op, sect, n);
		}
		if (ps == PSA_SUCCESS && rc == 0) {
			ps = psa_hash_finish(&op, digest, sizeof digest,
					     &dlen);
		} else {
			psa_hash_abort(&op);
		}
		nn_proto_identity_psa_unlock();
		if (rc) goto out;
		if (ps != PSA_SUCCESS ||
		    memcmp(digest, g_pending.sha256, SHA256_LEN) != 0) {
			printk("OTA: chunkdiff image verify FAILED\n");
			rc = -EBADMSG;
			goto out;
		}
	}

	printk("OTA chunkdiff complete in %u ms: %u skipped, %u local, %u downloaded (of %u blocks)\n",
	       (unsigned)(nn_osal_uptime_ms_32() - t0),
	       st_skip, st_local, st_dl, total_blocks);
	rc = 0;
out:
	if (sect)  k_heap_free(&s_ota_heap, sect);
	if (sums)  k_heap_free(&s_ota_heap, sums);
	if (reply) k_heap_free(&s_ota_heap, reply);
	return rc;
}

static int legacy_full_download(void);

int ota_client_download(void)
{
	if (!g_pending.valid) {
		NN_LOG_WRN("No pending update — run check first");
		return -EINVAL;
	}

	NN_LOG_INF("OTA download start: %s (%u B)",
		g_pending.version, g_pending.size);

	/* Suspend the deferred-logging thread for the entire flash-touching
	 * portion.  On ESP32-C6 the flash XIP path corrupts the logging
	 * thread's stack if both run concurrently — see
	 * feedback_esp32c6_flash_xip.md.  printk() is wired to bypass the
	 * log subsystem (CONFIG_LOG_PRINTK=n) so progress output still
	 * lands on UART0. */
	printk("OTA: suspending logging thread for flash ops\n");
	coap_log_thread_suspend();

#ifdef CONFIG_NODE_MGR_OTA_DELTA
	if (g_pending.patch_size > 0) {
		/* No blanket slot-1 pre-erase: the resumable stager keeps
		 * already-staged sectors and erases per-sector on (re)fetch,
		 * so an interrupted delta resumes instead of restarting. */
		int drc = delta_download();
		if (drc == 0) {
			return 0;
		}
		/* Transient mesh errors (-ETIMEDOUT stall, -EIO unrecoverable
		 * block) keep the staged progress — end the flow and let the
		 * next OTA hint RESUME the delta, rather than fall back to the
		 * much larger chunk-diff/full transfer on the same weak path.
		 * Only structural errors (bad sizes, ENOSPC, or a corrupt
		 * patch that fails the whole-patch sha) fall through. */
		if (drc == -ETIMEDOUT || drc == -EIO) {
			printk("OTA: delta interrupted (%d) — resume on next hint\n",
			       drc);
			return drc;
		}
		printk("OTA: delta unusable (%d) — falling back to chunk-diff\n",
		       drc);
	}
#endif

	int rc = chunkdiff_download();
	if (rc == 0) {
		/* Same deliberate non-resume as the legacy path — see the
		 * comment at the end of legacy_full_download(). */
		return 0;
	}
	printk("OTA: chunk-diff unavailable/failed (%d) — full download\n", rc);
	return legacy_full_download();
}

static int legacy_full_download(void)
{
	/* Pre-erase slot1 in one shot before the block loop. */
	printk("OTA: erasing slot1 (pre-erase)\n");
	uint32_t t0 = nn_osal_uptime_ms_32();
	int rc = nn_pal_dfu_erase_secondary();
	if (rc < 0) {
		printk("OTA: dfu_erase_secondary: %d\n", rc);
		coap_log_thread_resume();
		return rc;
	}
	printk("OTA: slot1 erased in %u ms\n",
	       (unsigned)(nn_osal_uptime_ms_32() - t0));

	g_dfu_ctx = nn_pal_dfu_ctx_alloc();
	if (!g_dfu_ctx) {
		printk("OTA: dfu_ctx_alloc failed\n");
		coap_log_thread_resume();
		return -ENOMEM;
	}
	rc = nn_pal_dfu_begin(g_dfu_ctx, g_pending.size);
	if (rc < 0) {
		printk("OTA: dfu_begin: %d\n", rc);
		nn_pal_dfu_ctx_free(g_dfu_ctx);
		g_dfu_ctx = NULL;
		coap_log_thread_resume();
		return rc;
	}
	printk("OTA: flash writer ready for secondary slot\n");

	/* Same RF/flash-controller settle window we used in the CoAP path.
	 * Empirically the very first post-erase request gets dropped on
	 * the C6 if we don't wait here. */
	nn_osal_sleep_ms(200);

	const uint32_t total_blocks =
		(g_pending.size + BLOCK_SIZE - 1) / BLOCK_SIZE;

	uint32_t block_num  = 0;
	uint32_t total_recv = 0;
	uint8_t *reply = k_heap_alloc(&s_ota_heap, BLOCK_REPLY_CAP,
				      K_MSEC(OTA_HEAP_ALLOC_TIMEOUT_MS));
	if (!reply) {
		printk("OTA: heap alloc reply buf (%u B) failed\n",
		       (unsigned)BLOCK_REPLY_CAP);
		coap_log_thread_resume();
		return -ENOMEM;
	}

	while (block_num < total_blocks) {
		const bool last_block = (block_num + 1 == total_blocks);
		uint32_t remaining = g_pending.size - total_recv;
		uint16_t want = (uint16_t)(remaining < BLOCK_SIZE
					   ? remaining : BLOCK_SIZE);

		uint8_t req[6];
		req[0] = (uint8_t)(block_num);
		req[1] = (uint8_t)(block_num >> 8);
		req[2] = (uint8_t)(block_num >> 16);
		req[3] = (uint8_t)(block_num >> 24);
		req[4] = (uint8_t)(want);
		req[5] = (uint8_t)(want >> 8);

		int rv = -ETIMEDOUT;
		size_t reply_len = 0;
		for (int retry = 0; retry <= OTA_BLOCK_RETRIES; retry++) {
			if (retry > 0) {
				widen_timeout();
				printk("OTA: block %u retry %d (timeout=%d ms)\n",
				       block_num, retry, g_request_timeout_ms);
				nn_osal_sleep_ms(50);
			}
			reply_len = BLOCK_REPLY_CAP;
			rv = nn_proto_client_request_d2h(
				NN_PROTO_CMD_OTA_BLOCK_REQ,
				req, sizeof(req),
				NN_PROTO_CMD_OTA_BLOCK,
				reply, &reply_len, g_request_timeout_ms);
			if (rv == 0 && reply_len >= 4) {
				uint32_t got_num =
					((uint32_t)reply[0]) |
					((uint32_t)reply[1] << 8) |
					((uint32_t)reply[2] << 16) |
					((uint32_t)reply[3] << 24);
				if (got_num == block_num) {
					break;   /* good reply */
				}
				printk("OTA: block %u: hub sent %u (mismatch)\n",
				       block_num, got_num);
				rv = -EBADMSG;
			}
			if (retry == OTA_BLOCK_RETRIES) {
				printk("OTA: block %u failed after %d retries: %d\n",
				       block_num, OTA_BLOCK_RETRIES, rv);
				k_heap_free(&s_ota_heap, reply);
				coap_log_thread_resume();
				return rv;
			}
		}

		const uint8_t *payload = reply + 4;
		size_t payload_len = reply_len - 4;
		if (payload_len == 0) {
			printk("OTA: block %u empty payload\n", block_num);
			k_heap_free(&s_ota_heap, reply);
			coap_log_thread_resume();
			return -ENODATA;
		}

		rc = nn_pal_dfu_write(g_dfu_ctx, payload, payload_len);
		if (rc < 0) {
			printk("OTA: block %u flash write: %d\n", block_num, rc);
			k_heap_free(&s_ota_heap, reply);
			coap_log_thread_resume();
			return rc;
		}
		if (last_block) {
			int frc = nn_pal_dfu_finalise(g_dfu_ctx);
			if (frc < 0) {
				printk("OTA: dfu_finalise: %d\n", frc);
				k_heap_free(&s_ota_heap, reply);
				coap_log_thread_resume();
				return frc;
			}
		}

		total_recv += payload_len;
		block_num++;

		if ((block_num % 200) == 0) {
			printk("OTA: %u / %u B (%u%%)\n",
			       total_recv, g_pending.size,
			       (total_recv * 100) / g_pending.size);
		}
	}

	printk("OTA download complete: %u B in %u blocks\n",
	       total_recv, block_num);
	k_heap_free(&s_ota_heap, reply);

	/* Whole-image sha256 verify BEFORE the caller arms.  Per-block
	 * receipt is not integrity: the image-key incident (2026-08-14)
	 * shipped a hub-side mismatch where every block arrived intact
	 * but the assembled image was a different release — it armed,
	 * "swapped successfully" onto the old version, and looped.  The
	 * delta and chunk-diff paths already verify; this closes the
	 * plain full-download path. */
	{
		uint8_t *scratch = k_heap_alloc(&s_ota_heap, 4096, K_NO_WAIT);
		uint8_t  dg[32];
		if (!scratch) {
			printk("OTA: verify alloc failed\n");
			return -ENOMEM;
		}
		int vrc = slot1_sha256(0, g_pending.size, scratch, dg);
		k_heap_free(&s_ota_heap, scratch);
		if (vrc) {
			printk("OTA: image verify read failed: %d\n", vrc);
			return vrc;
		}
		if (memcmp(dg, g_pending.sha256, SHA256_LEN) != 0) {
			printk("OTA: FULL-IMAGE sha256 MISMATCH — not arming\n");
			return -EBADMSG;
		}
		printk("OTA: full-image sha256 verified\n");
	}
	/* DELIBERATELY DO NOT resume the logging thread on success.
	 *
	 * The previous-success OTA flow (memory project_ota_ab_swap.md,
	 * 2026-04-20) documents that calling k_thread_resume on the
	 * deferred-logging thread after flash ops crashes with an
	 * Illegal-Instruction exception — the log thread's PC is
	 * mid-execution from flash that was XIP-disabled during the
	 * write loop, and the I-cache may be inconsistent.  The crash
	 * may not halt the OTA worker thread, but the resulting
	 * corrupted state can leave slot1's trailer half-written, which
	 * makes MCUboot's swap-scratch hang on the next boot.  We do
	 * the apply (sys_reboot) immediately after this returns, so the
	 * log thread doesn't need to be revived — the reboot cleans it
	 * all up.  Callers that DON'T immediately reboot (e.g. shell
	 * `mesh ota download` for manual inspection) should handle
	 * resume themselves. */
	return 0;
}

/* ── Apply / confirm ──────────────────────────────────────────────────────── */

int ota_client_apply(void)
{
	int rc = nn_pal_dfu_request_upgrade(false);
	if (rc < 0) {
		printk("OTA: boot_request_upgrade: %d\n", rc);
		return rc;
	}
	printk("OTA: test upgrade scheduled — rebooting...\n");
	nn_osal_sleep_ms(200);
	nn_osal_sys_reboot(NN_OSAL_REBOOT_COLD);
	return 0;
}

int ota_client_confirm(void)
{
	nn_pal_dfu_state_t dst = NN_PAL_DFU_STATE_UNKNOWN;
	(void)nn_pal_dfu_get_state(&dst);
	if (dst != NN_PAL_DFU_STATE_RUNNING_CONFIRMED) {
		int rc = nn_pal_dfu_confirm();
		if (rc < 0) {
			NN_LOG_ERR("boot_write_img_confirmed: %d", rc);
			return rc;
		}
		NN_LOG_INF("OTA: image confirmed (permanent)");
		coap_log_send("OTA: image confirmed");
	}
	return 0;
}
