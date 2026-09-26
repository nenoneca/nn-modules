/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_pal/mdns.h>

#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <nn_osal/osal.h>

#include <zephyr/net/hostname.h>
#include <zephyr/net/dns_sd.h>
#include <zephyr/sys/byteorder.h>

NN_OSAL_LOG_MODULE(nn_pal_mdns);

/* Zephyr's DNS-SD subsystem registers services via a linker section
 * (DNS_SD_REGISTER_SERVICE).  There is no clean runtime-registration
 * API upstream.  This backend therefore maintains a small pool of
 * statically-reserved slots; nn_pal_mdns_advertise() fills in the next
 * free slot and inserts it into the iterable section.  Cap is small
 * (4) because the project today uses ≤1 service per device. */

#define NN_PAL_MDNS_MAX_SVCS  4

#define INSTANCE_MAX  32
#define SERVICE_MAX   32
#define PROTO_MAX     8
#define TXT_MAX       128

struct slot {
    bool                in_use;
    char                instance[INSTANCE_MAX];
    char                service [SERVICE_MAX ];
    char                proto   [PROTO_MAX   ];
    char                domain[8];          /* always "local" */
    uint16_t            port_be;            /* network byte order */
    uint8_t             txt[TXT_MAX];
    size_t              txt_size;
    struct dns_sd_rec   rec;
};

static struct slot s_slots[NN_PAL_MDNS_MAX_SVCS];

/* Iterable section storage — Zephyr's DNS_SD subsystem walks all
 * `struct dns_sd_rec` entries in this section.  We pre-place pointers
 * to our slot records here; slots not yet filled get a benign empty
 * record. */
STRUCT_SECTION_ITERABLE(dns_sd_rec, _nn_pal_mdns_rec_0);
STRUCT_SECTION_ITERABLE(dns_sd_rec, _nn_pal_mdns_rec_1);
STRUCT_SECTION_ITERABLE(dns_sd_rec, _nn_pal_mdns_rec_2);
STRUCT_SECTION_ITERABLE(dns_sd_rec, _nn_pal_mdns_rec_3);

static struct dns_sd_rec *s_section_recs[NN_PAL_MDNS_MAX_SVCS] = {
    &_nn_pal_mdns_rec_0,
    &_nn_pal_mdns_rec_1,
    &_nn_pal_mdns_rec_2,
    &_nn_pal_mdns_rec_3,
};

int nn_pal_mdns_init(void)
{
    /* Section records start as zeroed; that's "empty rec" which the
     * responder ignores. */
    return 0;
}

void nn_pal_mdns_shutdown(void)
{
    for (int i = 0; i < NN_PAL_MDNS_MAX_SVCS; i++) {
        memset(s_section_recs[i], 0, sizeof *s_section_recs[i]);
        s_slots[i].in_use = false;
    }
}

int nn_pal_mdns_set_hostname(const char *host)
{
    if (!host || !*host) return -EINVAL;
    return net_hostname_set((char *)host, strlen(host));
}

/* Build a TXT record blob from key=val pairs.  DNS-SD format:
 *   <len><k=v><len><k=v>...
 * where each <len> is a single byte preceding the entry.  Returns the
 * total length written or negative errno. */
static int build_txt(const nn_pal_mdns_txt_t *txt, size_t n,
                     uint8_t *out, size_t cap)
{
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        size_t klen = txt[i].key ? strlen(txt[i].key) : 0;
        size_t vlen = txt[i].val ? strlen(txt[i].val) : 0;
        if (klen == 0) continue;
        size_t entry = klen + (vlen ? 1 + vlen : 0);
        if (entry > 255) return -EMSGSIZE;
        if (off + 1 + entry > cap) return -ENOMEM;
        out[off++] = (uint8_t)entry;
        memcpy(out + off, txt[i].key, klen);
        off += klen;
        if (vlen) {
            out[off++] = '=';
            memcpy(out + off, txt[i].val, vlen);
            off += vlen;
        }
    }
    return (int)off;
}

/* Split "_meshcop._udp" → service="_meshcop", proto="_udp". */
static int split_service(const char *full, char *service, size_t srv_cap,
                         char *proto,   size_t pro_cap)
{
    const char *dot = strchr(full, '.');
    if (!dot) return -EINVAL;
    size_t slen = (size_t)(dot - full);
    size_t plen = strlen(dot + 1);
    if (slen == 0 || slen >= srv_cap) return -ENOMEM;
    if (plen == 0 || plen >= pro_cap) return -ENOMEM;
    memcpy(service, full, slen); service[slen] = '\0';
    memcpy(proto, dot + 1, plen); proto[plen] = '\0';
    return 0;
}

int nn_pal_mdns_advertise(const char *service_type,
                          uint16_t port,
                          const nn_pal_mdns_txt_t *txt, size_t txt_count,
                          nn_pal_mdns_service_t *out_handle)
{
    if (!service_type || !out_handle) return -EINVAL;

    /* Find free slot. */
    int slot = -1;
    for (int i = 0; i < NN_PAL_MDNS_MAX_SVCS; i++) {
        if (!s_slots[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -ENOMEM;

    struct slot *s = &s_slots[slot];

    /* Split "_xxx._yyy" → service / proto. */
    int rv = split_service(service_type, s->service, sizeof s->service,
                           s->proto, sizeof s->proto);
    if (rv) return rv;

    /* Instance = hostname (Zephyr's CONFIG_NET_HOSTNAME).  We rely on
     * the caller having set the hostname first via nn_pal_mdns_set_hostname. */
    {
        const char *host = net_hostname_get();
        size_t hl = host ? strlen(host) : 0;
        if (hl == 0 || hl >= sizeof s->instance) return -ENOENT;
        memcpy(s->instance, host, hl + 1);
    }
    memcpy(s->domain, "local", 6);
    s->port_be = sys_cpu_to_be16(port);

    int txtlen = build_txt(txt, txt_count, s->txt, sizeof s->txt);
    if (txtlen < 0) return txtlen;
    s->txt_size = (size_t)txtlen;

    struct dns_sd_rec *rec = s_section_recs[slot];
    rec->instance  = s->instance;
    rec->service   = s->service;
    rec->proto     = s->proto;
    rec->domain    = s->domain;
    rec->port      = &s->port_be;   /* dns_sd_rec.port is const uint16_t* (BE) */
    rec->text      = (char *)s->txt;
    rec->text_size = s->txt_size;

    s->in_use = true;
    *out_handle = slot + 1;   /* 0 reserved for invalid */

    NN_LOG_INF("advertise: %s.%s.%s on port %u (slot %d, %zu B TXT)",
               s->instance, s->service, s->proto, port, slot, s->txt_size);
    return 0;
}

int nn_pal_mdns_unpublish(nn_pal_mdns_service_t handle)
{
    if (handle < 1 || handle > NN_PAL_MDNS_MAX_SVCS) return -EINVAL;
    int slot = handle - 1;
    if (!s_slots[slot].in_use) return -ENOENT;
    memset(s_section_recs[slot], 0, sizeof *s_section_recs[slot]);
    s_slots[slot].in_use = false;
    return 0;
}
