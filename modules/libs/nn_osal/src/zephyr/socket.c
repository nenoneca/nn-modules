/* SPDX-License-Identifier: Apache-2.0 */

#include <nn_osal/socket.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#include <errno.h>

/* OSAL flag → POSIX flag bit mapping. */
static int to_msg_flags(int f)
{
    int out = 0;
    if (f & NN_OSAL_MSG_DONTWAIT) out |= ZSOCK_MSG_DONTWAIT;
    if (f & NN_OSAL_MSG_PEEK)     out |= ZSOCK_MSG_PEEK;
    if (f & NN_OSAL_MSG_TRUNC)    out |= ZSOCK_MSG_TRUNC;
    return out;
}

static void to_zaddr(struct sockaddr_in6 *z,
                     const nn_osal_sockaddr_in6_t *o)
{
    memset(z, 0, sizeof *z);
    z->sin6_family   = AF_INET6;
    z->sin6_port     = htons(o->port);
    z->sin6_scope_id = o->scope_id;
    memcpy(&z->sin6_addr, o->addr, 16);
}

static void from_zaddr(nn_osal_sockaddr_in6_t *o,
                       const struct sockaddr_in6 *z)
{
    memset(o, 0, sizeof *o);
    o->port     = ntohs(z->sin6_port);
    o->scope_id = z->sin6_scope_id;
    memcpy(o->addr, &z->sin6_addr, 16);
}

nn_osal_socket_t nn_osal_socket(int domain, int type, int proto)
{
    int zd = (domain == NN_OSAL_AF_INET6) ? AF_INET6 : -1;
    int zt = (type == NN_OSAL_SOCK_DGRAM) ? SOCK_DGRAM
           : (type == NN_OSAL_SOCK_STREAM) ? SOCK_STREAM : -1;
    int zp = (proto == NN_OSAL_IPPROTO_UDP) ? IPPROTO_UDP
           : (proto == NN_OSAL_IPPROTO_TCP) ? IPPROTO_TCP : 0;
    return zsock_socket(zd, zt, zp);
}

int nn_osal_close(nn_osal_socket_t s)              { return zsock_close(s); }

int nn_osal_bind(nn_osal_socket_t s,
                 const nn_osal_sockaddr_in6_t *a)
{
    struct sockaddr_in6 z;
    to_zaddr(&z, a);
    return zsock_bind(s, (struct sockaddr *)&z, sizeof z);
}

int nn_osal_connect(nn_osal_socket_t s,
                    const nn_osal_sockaddr_in6_t *a)
{
    struct sockaddr_in6 z;
    to_zaddr(&z, a);
    return zsock_connect(s, (struct sockaddr *)&z, sizeof z);
}

int nn_osal_listen(nn_osal_socket_t s, int backlog)
{
    return zsock_listen(s, backlog);
}

nn_osal_socket_t nn_osal_accept(nn_osal_socket_t s,
                                nn_osal_sockaddr_in6_t *peer)
{
    struct sockaddr_in6 z;
    socklen_t zl = sizeof z;
    int fd = zsock_accept(s, (struct sockaddr *)&z, &zl);
    if (fd >= 0 && peer) from_zaddr(peer, &z);
    return fd;
}

int nn_osal_send(nn_osal_socket_t s, const void *b, size_t n, int f)
{
    return zsock_send(s, b, n, to_msg_flags(f));
}

int nn_osal_sendto(nn_osal_socket_t s, const void *b, size_t n, int f,
                   const nn_osal_sockaddr_in6_t *d)
{
    struct sockaddr_in6 z;
    to_zaddr(&z, d);
    return zsock_sendto(s, b, n, to_msg_flags(f),
                        (struct sockaddr *)&z, sizeof z);
}

int nn_osal_recv(nn_osal_socket_t s, void *b, size_t n, int f)
{
    return zsock_recv(s, b, n, to_msg_flags(f));
}

int nn_osal_recvfrom(nn_osal_socket_t s, void *b, size_t n, int f,
                     nn_osal_sockaddr_in6_t *src)
{
    struct sockaddr_in6 z;
    socklen_t zl = sizeof z;
    int r = zsock_recvfrom(s, b, n, to_msg_flags(f),
                           (struct sockaddr *)&z, &zl);
    if (r >= 0 && src) from_zaddr(src, &z);
    return r;
}

int nn_osal_setsockopt(nn_osal_socket_t s, int level, int optname,
                       const void *val, size_t len)
{
    int zlevel = (level == NN_OSAL_SOL_SOCKET) ? SOL_SOCKET : level;
    int zoptn  = optname;
    /* SO_* names match POSIX values for the ones we expose; if you add
     * new ones, translate here. */
    return zsock_setsockopt(s, zlevel, zoptn, val, len);
}

int nn_osal_poll(nn_osal_pollfd_t *fds, size_t nfds, int timeout_ms)
{
    /* Thin translation; OSAL and zsock_pollfd shape is the same. */
    struct zsock_pollfd *zfds =
        (struct zsock_pollfd *)k_malloc(sizeof(*zfds) * nfds);
    if (!zfds) return -ENOMEM;
    for (size_t i = 0; i < nfds; i++) {
        zfds[i].fd = fds[i].sock;
        zfds[i].events = 0;
        if (fds[i].events & NN_OSAL_POLLIN)  zfds[i].events |= ZSOCK_POLLIN;
        if (fds[i].events & NN_OSAL_POLLOUT) zfds[i].events |= ZSOCK_POLLOUT;
    }
    int r = zsock_poll(zfds, nfds, timeout_ms);
    for (size_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (zfds[i].revents & ZSOCK_POLLIN)  fds[i].revents |= NN_OSAL_POLLIN;
        if (zfds[i].revents & ZSOCK_POLLOUT) fds[i].revents |= NN_OSAL_POLLOUT;
        if (zfds[i].revents & ZSOCK_POLLERR) fds[i].revents |= NN_OSAL_POLLERR;
        if (zfds[i].revents & ZSOCK_POLLHUP) fds[i].revents |= NN_OSAL_POLLHUP;
    }
    k_free(zfds);
    return r;
}

int nn_osal_inet_pton6(const char *str, uint8_t out_addr[16])
{
    if (!str || !out_addr) return -EINVAL;
    int rv = zsock_inet_pton(AF_INET6, str, out_addr);
    if (rv == 1) return 0;
    if (rv == 0) return -EINVAL;
    return -errno;
}

int nn_osal_inet_ntop6(const uint8_t addr[16], char *out, size_t out_cap)
{
    if (!addr || !out || out_cap < NN_OSAL_INET6_ADDRSTRLEN) return -EINVAL;
    if (zsock_inet_ntop(AF_INET6, addr, out, out_cap) == NULL) {
        return -errno;
    }
    return 0;
}
