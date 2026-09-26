/* SPDX-License-Identifier: Apache-2.0 */
#include "nn_timesync/nn_timesync.h"
#if CONFIG_NN_TIMESYNC_HAS_LINK
/* The P4<->C6 offset sub-protocol runs over nn_link (C6 media-network only).
 * The esp-hosted P4 host owns the clock directly, so it has no link peer. */
#include "nn_link/nn_link.h"
#include "nn_link/nn_time.h"
#endif
#include <nn_osal/time.h>
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_timesync);
#include <stdio.h>
#include <string.h>


static bool     s_synced;          /* got an absolute time from the hub        */
static uint64_t s_epoch_base;      /* hub epoch ms at the last sync            */
static uint64_t s_timer_base_ms;   /* local uptime ms at the last sync        */

static bool     s_p4_synced;       /* measured the P4<->abs offset             */
static int64_t  s_p4_offset_ms;    /* abs_ms - p4_timer_ms                     */
static uint32_t s_p4_reqs;

static inline uint64_t now_ms(void) { return (uint64_t)nn_osal_uptime_ms(); }

void nn_timesync_init(void) { /* zero-init statics suffice */ }

uint64_t nn_timesync_abs_ms(void)
{
    if (!s_synced) return 0;
    return s_epoch_base + (now_ms() - s_timer_base_ms);
}

bool nn_timesync_synced(void) { return s_synced; }

static void notify_p4(void)
{
#if CONFIG_NN_TIMESYNC_HAS_LINK
    nn_time_hdr_t h = { .magic = NN_TIME_MAGIC, .type = NN_TIME_NOTIFY, .rsv = 0, .p4_timer_ms = 0 };
    nn_link_send((const uint8_t *)&h, NN_TIME_HDR_LEN);
#endif
}

void nn_timesync_set_epoch(uint64_t epoch_ms)
{
    s_epoch_base = epoch_ms;
    s_timer_base_ms = now_ms();
    s_synced = true;
    NN_LOG_INF("hub time set: epoch=%llu ms", (unsigned long long)epoch_ms);
    notify_p4();                     /* ask the P4 to (re)sync its offset */
}

uint64_t nn_timesync_p4_to_abs(uint32_t p4_ts_ms)
{
    if (s_p4_synced) return (uint64_t)((int64_t)p4_ts_ms + s_p4_offset_ms);
    if (s_synced)    return nn_timesync_abs_ms();   /* fallback: stamp at relay */
    return p4_ts_ms;                                /* unsynced: pass through   */
}

void nn_timesync_on_p4_msg(const uint8_t *data, size_t len)
{
#if CONFIG_NN_TIMESYNC_HAS_LINK
    if (len < (size_t)NN_TIME_HDR_LEN) return;
    const nn_time_hdr_t *h = (const nn_time_hdr_t *)data;
    if (h->type != NN_TIME_REQ) return;
    /* The P4 echoed its current esp_timer ms; map it to our absolute clock so
     * we can rewrite every relayed media stamp into absolute hub time. */
    uint64_t abs = nn_timesync_abs_ms();
    if (abs == 0) return;            /* not hub-synced yet */
    s_p4_offset_ms = (int64_t)abs - (int64_t)h->p4_timer_ms;
    s_p4_synced = true;
    s_p4_reqs++;
    NN_LOG_INF("P4 offset measured: %lld ms (abs=%llu p4=%lu)",
             (long long)s_p4_offset_ms, (unsigned long long)abs, (unsigned long)h->p4_timer_ms);
#else
    (void)data; (void)len;
#endif
}

void nn_timesync_status(char *out, size_t n)
{
    snprintf(out, n, "synced=%d abs=%llu p4_synced=%d offset=%lld reqs=%lu",
             s_synced, (unsigned long long)nn_timesync_abs_ms(),
             s_p4_synced, (long long)s_p4_offset_ms, (unsigned long)s_p4_reqs);
}
