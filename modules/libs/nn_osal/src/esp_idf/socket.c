/* SPDX-License-Identifier: Apache-2.0 */
/* nn_osal socket backend — ESP-IDF (lwip BSD sockets, IPv6). */
#include "nn_osal/socket.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <string.h>
#include <errno.h>

static void to_sin6(const nn_osal_sockaddr_in6_t *a, struct sockaddr_in6 *s)
{
    memset(s, 0, sizeof *s);
    s->sin6_family   = AF_INET6;
    s->sin6_port     = lwip_htons(a->port);
    s->sin6_scope_id = a->scope_id;
    memcpy(&s->sin6_addr, a->addr, 16);
}

static void from_sin6(const struct sockaddr_in6 *s, nn_osal_sockaddr_in6_t *a)
{
    a->port     = lwip_ntohs(s->sin6_port);
    a->scope_id = s->sin6_scope_id;
    memcpy(a->addr, &s->sin6_addr, 16);
}

nn_osal_socket_t nn_osal_socket(int domain, int type, int protocol)
{
    (void)domain;   /* OSAL is v6-only */
    return lwip_socket(AF_INET6, type, protocol);
}

int nn_osal_close(nn_osal_socket_t sock) { return lwip_close(sock); }

int nn_osal_bind(nn_osal_socket_t sock, const nn_osal_sockaddr_in6_t *addr)
{
    struct sockaddr_in6 s; to_sin6(addr, &s);
    return lwip_bind(sock, (struct sockaddr *)&s, sizeof s);
}

int nn_osal_connect(nn_osal_socket_t sock, const nn_osal_sockaddr_in6_t *addr)
{
    struct sockaddr_in6 s; to_sin6(addr, &s);
    return lwip_connect(sock, (struct sockaddr *)&s, sizeof s);
}

int nn_osal_listen(nn_osal_socket_t sock, int backlog) { return lwip_listen(sock, backlog); }

nn_osal_socket_t nn_osal_accept(nn_osal_socket_t sock, nn_osal_sockaddr_in6_t *peer)
{
    struct sockaddr_in6 s; socklen_t sl = sizeof s;
    int fd = lwip_accept(sock, (struct sockaddr *)&s, &sl);
    if (fd >= 0 && peer) from_sin6(&s, peer);
    return fd;
}

int nn_osal_send(nn_osal_socket_t sock, const void *buf, size_t len, int flags)
{
    return lwip_send(sock, buf, len, flags);
}

int nn_osal_sendto(nn_osal_socket_t sock, const void *buf, size_t len, int flags,
                   const nn_osal_sockaddr_in6_t *dst)
{
    struct sockaddr_in6 s; to_sin6(dst, &s);
    return lwip_sendto(sock, buf, len, flags, (struct sockaddr *)&s, sizeof s);
}

int nn_osal_recv(nn_osal_socket_t sock, void *buf, size_t len, int flags)
{
    return lwip_recv(sock, buf, len, flags);
}

int nn_osal_recvfrom(nn_osal_socket_t sock, void *buf, size_t len, int flags,
                     nn_osal_sockaddr_in6_t *src)
{
    struct sockaddr_in6 s; socklen_t sl = sizeof s;
    int n = lwip_recvfrom(sock, buf, len, flags, (struct sockaddr *)&s, &sl);
    if (n >= 0 && src) from_sin6(&s, src);
    return n;
}

int nn_osal_setsockopt(nn_osal_socket_t sock, int level, int optname,
                       const void *optval, size_t optlen)
{
    if (level == NN_OSAL_SOL_SOCKET) level = SOL_SOCKET;
    switch (optname) {
    case NN_OSAL_SO_RCVTIMEO:  optname = SO_RCVTIMEO;  break;
    case NN_OSAL_SO_REUSEADDR: optname = SO_REUSEADDR; break;
    default: break;
    }
    return lwip_setsockopt(sock, level, optname, optval, (socklen_t)optlen);
}

int nn_osal_poll(nn_osal_pollfd_t *fds, size_t nfds, int timeout_ms)
{
    struct pollfd pf[16];
    if (nfds > 16) { errno = EINVAL; return -1; }
    for (size_t i = 0; i < nfds; i++) {
        pf[i].fd = fds[i].sock;
        pf[i].events = 0;
        if (fds[i].events & NN_OSAL_POLLIN)  pf[i].events |= POLLIN;
        if (fds[i].events & NN_OSAL_POLLOUT) pf[i].events |= POLLOUT;
        pf[i].revents = 0;
    }
    int r = lwip_poll(pf, nfds, timeout_ms);
    for (size_t i = 0; i < nfds; i++) {
        uint32_t rv = 0;
        if (pf[i].revents & POLLIN)  rv |= NN_OSAL_POLLIN;
        if (pf[i].revents & POLLOUT) rv |= NN_OSAL_POLLOUT;
        if (pf[i].revents & POLLERR) rv |= NN_OSAL_POLLERR;
        if (pf[i].revents & POLLHUP) rv |= NN_OSAL_POLLHUP;
        fds[i].revents = rv;
    }
    return r;
}

int nn_osal_inet_pton6(const char *str, uint8_t out_addr[16])
{
    return inet_pton(AF_INET6, str, out_addr) == 1 ? 0 : -EINVAL;
}

int nn_osal_inet_ntop6(const uint8_t addr[16], char *out, size_t out_cap)
{
    return inet_ntop(AF_INET6, addr, out, out_cap) ? 0 : -EINVAL;
}
