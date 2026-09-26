/* SPDX-License-Identifier: Apache-2.0 */
/* nn_sectun — converged secure session over nn_osal + nn_crypto. */
#include "nn_sectun/nn_sectun.h"
#include "nn_crypto/nn_crypto.h"
#include "nn_osal/socket.h"
#include "nn_osal/log.h"
#include <string.h>
#include <errno.h>

NN_OSAL_LOG_MODULE("nn_sectun");

#define VERSION 1
static const uint8_t INFO[] = "nn-sectun-v1";
#define INFO_LEN (sizeof(INFO) - 1)

static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        int w = nn_osal_send(fd, p, n, 0);
        if (w <= 0) return -EIO;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, uint8_t *p, size_t n)
{
    while (n) {
        int r = nn_osal_recv(fd, p, n, 0);
        if (r <= 0) return -EIO;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static void make_nonce(uint8_t nonce[12], uint64_t ctr)
{
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(ctr >> (56 - 8 * i));
}

int nn_sectun_client_handshake(nn_sectun_t *s, int fd, const uint8_t peer_pub[32])
{
    uint8_t eph_pub[32], eph_priv[32], dev_pub[32];
    if (nn_crypto_gen_x25519(eph_pub, eph_priv) != 0) return -EIO;
    nn_crypto_device_pub(dev_pub);

    uint8_t secret[64], okm[64];
    int rc = -EIO;
    if (nn_crypto_x25519(eph_priv, peer_pub, secret) != 0) goto done;
    if (nn_crypto_device_x25519(peer_pub, secret + 32) != 0) goto done;
    if (nn_crypto_hkdf(secret, 64, eph_pub, 32, INFO, INFO_LEN, okm, 64) != 0) goto done;

    memset(s, 0, sizeof *s);
    s->fd = fd;
    memcpy(s->k_tx, okm, 32);
    memcpy(s->k_rx, okm + 32, 32);

    uint8_t hello[4 + 2 + 32 + 32];
    hello[0] = 'N'; hello[1] = 'N'; hello[2] = 'S'; hello[3] = '1';
    hello[4] = VERSION; hello[5] = 0;
    memcpy(hello + 6, eph_pub, 32);
    memcpy(hello + 38, dev_pub, 32);
    rc = write_all(fd, hello, sizeof hello);
    if (rc == 0)
        NN_LOG_INF("secure session up (peer %02x%02x%02x%02x..)",
                   peer_pub[0], peer_pub[1], peer_pub[2], peer_pub[3]);
done:
    memset(secret, 0, sizeof secret);
    memset(eph_priv, 0, sizeof eph_priv);
    memset(okm, 0, sizeof okm);
    return rc;
}

static int send_one(nn_sectun_t *s, const uint8_t *data, size_t len)
{
    uint8_t nonce[12];
    make_nonce(nonce, s->ctr_tx);
    uint8_t ct[NN_SECTUN_RECORD_MAX + 16];
    size_t ct_len = sizeof ct;
    if (nn_crypto_aesgcm(false, s->k_tx, nonce, NULL, 0, data, len, ct, sizeof ct, &ct_len) != 0)
        return -EIO;
    s->ctr_tx++;
    uint8_t hdr[4] = { (uint8_t)(ct_len >> 24), (uint8_t)(ct_len >> 16),
                       (uint8_t)(ct_len >> 8), (uint8_t)ct_len };
    if (write_all(s->fd, hdr, 4) != 0) return -EIO;
    return write_all(s->fd, ct, ct_len);
}

int nn_sectun_send(nn_sectun_t *s, const uint8_t *data, size_t len)
{
    while (len) {
        size_t chunk = len > NN_SECTUN_RECORD_MAX ? NN_SECTUN_RECORD_MAX : len;
        int rc = send_one(s, data, chunk);
        if (rc != 0) return rc;
        data += chunk; len -= chunk;
    }
    return 0;
}

int nn_sectun_recv(nn_sectun_t *s, uint8_t *out, size_t cap, size_t *out_len)
{
    uint8_t hdr[4];
    if (read_all(s->fd, hdr, 4) != 0) return -EIO;
    size_t ct_len = (size_t)hdr[0] << 24 | (size_t)hdr[1] << 16 |
                    (size_t)hdr[2] << 8 | hdr[3];
    if (ct_len < 16 || ct_len > NN_SECTUN_RECORD_MAX + 16) return -EBADMSG;
    uint8_t ct[NN_SECTUN_RECORD_MAX + 16];
    if (read_all(s->fd, ct, ct_len) != 0) return -EIO;
    uint8_t nonce[12];
    make_nonce(nonce, s->ctr_rx);
    if (nn_crypto_aesgcm(true, s->k_rx, nonce, NULL, 0, ct, ct_len, out, cap, out_len) != 0)
        return -EACCES;
    s->ctr_rx++;
    return 0;
}
