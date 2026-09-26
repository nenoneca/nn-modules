/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal socket backend — POSIX (BSD sockets, IPv6).  Mirrors the lwip
 * backend byte-for-byte in semantics; the OSAL surface is v6-only. */
#include "nn_osal/socket.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <assert.h>

/* The OSAL socket-type and poll-event values happen to equal the Linux native
 * ones, so the calls below pass them through.  PROVE it at compile time — if
 * a libc ever disagrees, this backend must start mapping. */
_Static_assert(NN_OSAL_SOCK_STREAM == SOCK_STREAM, "map needed");
_Static_assert(NN_OSAL_SOCK_DGRAM  == SOCK_DGRAM,  "map needed");
_Static_assert(NN_OSAL_POLLIN      == POLLIN,      "map needed");

static void to_sin6(const nn_osal_sockaddr_in6_t *a, struct sockaddr_in6 *s)
{
    memset(s, 0, sizeof *s);
    s->sin6_family   = AF_INET6;
    s->sin6_port     = htons(a->port);
    s->sin6_scope_id = a->scope_id;
    memcpy(&s->sin6_addr, a->addr, 16);
}

static void from_sin6(const struct sockaddr_in6 *s, nn_osal_sockaddr_in6_t *a)
{
    a->port     = ntohs(s->sin6_port);
    a->scope_id = s->sin6_scope_id;
    memcpy(a->addr, &s->sin6_addr, 16);
}

nn_osal_socket_t nn_osal_socket(int domain, int type, int protocol)
{
    (void)domain;
    return socket(AF_INET6, type, protocol);
}

int nn_osal_close(nn_osal_socket_t sock) { return close(sock); }

int nn_osal_bind(nn_osal_socket_t sock, const nn_osal_sockaddr_in6_t *addr)
{
    struct sockaddr_in6 s; to_sin6(addr, &s);
    return bind(sock, (struct sockaddr *)&s, sizeof s);
}

int nn_osal_connect(nn_osal_socket_t sock, const nn_osal_sockaddr_in6_t *addr)
{
    struct sockaddr_in6 s; to_sin6(addr, &s);
    return connect(sock, (struct sockaddr *)&s, sizeof s);
}

int nn_osal_listen(nn_osal_socket_t sock, int backlog) { return listen(sock, backlog); }

nn_osal_socket_t nn_osal_accept(nn_osal_socket_t sock, nn_osal_sockaddr_in6_t *addr)
{
    struct sockaddr_in6 s; socklen_t sl = sizeof s;
    int fd = accept(sock, (struct sockaddr *)&s, &sl);
    if (fd >= 0 && addr) from_sin6(&s, addr);
    return fd;
}

int nn_osal_send(nn_osal_socket_t sock, const void *buf, size_t len, int flags)
{
    return (int)send(sock, buf, len, flags);
}

int nn_osal_sendto(nn_osal_socket_t sock, const void *buf, size_t len, int flags,
                   const nn_osal_sockaddr_in6_t *dst)
{
    struct sockaddr_in6 s; to_sin6(dst, &s);
    return (int)sendto(sock, buf, len, flags, (struct sockaddr *)&s, sizeof s);
}

int nn_osal_recv(nn_osal_socket_t sock, void *buf, size_t len, int flags)
{
    return (int)recv(sock, buf, len, flags);
}

int nn_osal_recvfrom(nn_osal_socket_t sock, void *buf, size_t len, int flags,
                     nn_osal_sockaddr_in6_t *src)
{
    struct sockaddr_in6 s; socklen_t sl = sizeof s;
    int r = (int)recvfrom(sock, buf, len, flags, (struct sockaddr *)&s, &sl);
    if (r >= 0 && src) from_sin6(&s, src);
    return r;
}

int nn_osal_setsockopt(nn_osal_socket_t sock, int level, int optname,
                       const void *optval, size_t optlen)
{
    return setsockopt(sock, level, optname, optval, optlen);
}

int nn_osal_poll(nn_osal_pollfd_t *fds, size_t nfds, int timeout_ms)
{
    struct pollfd pf[16];
    if (nfds > 16) return -1;
    for (size_t i = 0; i < nfds; i++) {
        pf[i].fd = fds[i].sock;
        pf[i].events = (short)fds[i].events;
        pf[i].revents = 0;
    }
    int r = poll(pf, nfds, timeout_ms);
    for (size_t i = 0; i < nfds; i++)
        fds[i].revents = (uint32_t)pf[i].revents;
    return r;
}

int nn_osal_inet_pton6(const char *str, uint8_t out_addr[16])
{
    return inet_pton(AF_INET6, str, out_addr) == 1 ? 0 : -1;
}

int nn_osal_inet_ntop6(const uint8_t addr[16], char *out, size_t out_cap)
{
    return inet_ntop(AF_INET6, addr, out, out_cap) ? 0 : -1;
}
