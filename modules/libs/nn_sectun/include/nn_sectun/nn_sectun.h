/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * nn_sectun — authenticated encrypted session over a connected socket
 * (converged single source; builds on Zephyr + ESP-IDF via nn_osal + nn_crypto).
 *
 * Protocol "NNS1" (client = device source, server = video/control service):
 *   HELLO  = "NNS1" u8 ver=1 u8 flags=0 eph_pub[32] dev_pub[32]
 *   keys   = HKDF-SHA256(X25519(eph,peer) || X25519(dev,peer),
 *                        salt=eph_pub, info="nn-sectun-v1", 64)
 *            -> k_c2s[0:32], k_s2c[32:64]
 *   record = u32 BE ct_len, then AES-256-GCM ciphertext (incl 16B tag);
 *            nonce = 4 zero || u64 BE per-direction counter (not on the wire).
 *
 * The fd is an nn_osal socket descriptor (== platform fd); all I/O goes through
 * nn_osal_send/recv.  Returns 0 / negative errno.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define NN_SECTUN_RECORD_MAX  4096
#define NN_SECTUN_OVERHEAD    (4 + 16)

typedef struct {
    int      fd;
    uint8_t  k_tx[32];
    uint8_t  k_rx[32];
    uint64_t ctr_tx;
    uint64_t ctr_rx;
} nn_sectun_t;

int nn_sectun_client_handshake(nn_sectun_t *s, int fd, const uint8_t peer_pub[32]);
int nn_sectun_send(nn_sectun_t *s, const uint8_t *data, size_t len);
int nn_sectun_recv(nn_sectun_t *s, uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
