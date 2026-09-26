/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * nn_osal/socket.h — BSD-style sockets.
 *
 * Mirrors POSIX zsock_*  (Zephyr) / socket(2) (Linux) but uses fixed
 * typedefs so callers don't pull in headers from the backend's libc.
 * Only the subset nn libs actually use is exposed; expand as needed.
 *
 * Address family:  NN_OSAL_AF_INET6 only (the nn project is v6-only).
 * Socket types:    NN_OSAL_SOCK_DGRAM (UDP) and NN_OSAL_SOCK_STREAM (TCP).
 */

#define NN_OSAL_AF_INET6      10        /* matches Linux/POSIX value */
#define NN_OSAL_SOCK_DGRAM    2
#define NN_OSAL_SOCK_STREAM   1
#define NN_OSAL_IPPROTO_UDP   17
#define NN_OSAL_IPPROTO_TCP   6

/* MSG_* flag bits — recv side. */
#define NN_OSAL_MSG_DONTWAIT  (1 << 0)
#define NN_OSAL_MSG_PEEK      (1 << 1)
#define NN_OSAL_MSG_TRUNC     (1 << 2)

/* Setsockopt option numbers.  Subset used by nn libs. */
#define NN_OSAL_SOL_SOCKET    0xFFFF
#define NN_OSAL_SO_RCVTIMEO   20
#define NN_OSAL_SO_REUSEADDR  2

typedef int nn_osal_socket_t;   /* < 0 = invalid */

typedef struct {
    uint8_t addr[16];   /* IPv6 address in network byte order */
    uint16_t port;      /* host byte order, OSAL handles htons internally */
    uint32_t scope_id;  /* link-local scope; 0 for global */
} nn_osal_sockaddr_in6_t;

/* ── lifecycle ─────────────────────────────────────────────────────── */

nn_osal_socket_t nn_osal_socket(int domain, int type, int protocol);
int nn_osal_close(nn_osal_socket_t sock);

/* ── data plane ────────────────────────────────────────────────────── */

int nn_osal_bind(nn_osal_socket_t sock,
                 const nn_osal_sockaddr_in6_t *addr);
int nn_osal_connect(nn_osal_socket_t sock,
                    const nn_osal_sockaddr_in6_t *addr);
int nn_osal_listen(nn_osal_socket_t sock, int backlog);
nn_osal_socket_t nn_osal_accept(nn_osal_socket_t sock,
                                nn_osal_sockaddr_in6_t *peer);

int nn_osal_send(nn_osal_socket_t sock,
                 const void *buf, size_t len, int flags);
int nn_osal_sendto(nn_osal_socket_t sock,
                   const void *buf, size_t len, int flags,
                   const nn_osal_sockaddr_in6_t *dst);
int nn_osal_recv(nn_osal_socket_t sock,
                 void *buf, size_t len, int flags);
int nn_osal_recvfrom(nn_osal_socket_t sock,
                     void *buf, size_t len, int flags,
                     nn_osal_sockaddr_in6_t *src);

/* ── options + poll ────────────────────────────────────────────────── */

int nn_osal_setsockopt(nn_osal_socket_t sock, int level, int optname,
                       const void *optval, size_t optlen);

#define NN_OSAL_POLLIN   (1 << 0)
#define NN_OSAL_POLLOUT  (1 << 1)
#define NN_OSAL_POLLERR  (1 << 2)
#define NN_OSAL_POLLHUP  (1 << 3)

typedef struct {
    nn_osal_socket_t sock;
    uint32_t events;
    uint32_t revents;
} nn_osal_pollfd_t;

int nn_osal_poll(nn_osal_pollfd_t *fds, size_t nfds, int timeout_ms);

/* ── address parsing/formatting ────────────────────────────────── */

/* Maximum text length of an IPv6 address: "ffff:ffff:...:ffff" + scope. */
#define NN_OSAL_INET6_ADDRSTRLEN 46

/* Parse an IPv6 address string into 16 bytes (network byte order).
 * Accepts the standard "::1" / "ff02::1" / "2001:db8::1" forms.
 * Returns 0 on success, negative errno on failure. */
int nn_osal_inet_pton6(const char *str, uint8_t out_addr[16]);

/* Format 16-byte IPv6 address into a string buffer.  out_cap should
 * be ≥ NN_OSAL_INET6_ADDRSTRLEN. */
int nn_osal_inet_ntop6(const uint8_t addr[16], char *out, size_t out_cap);
