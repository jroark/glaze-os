/*
 * BSD sockets over the network server's protocol (glaze/net.h), which
 * runs them on lwIP's sockets. A descriptor is a net socket handle; the
 * BSD-only state (non-blocking mode, timeouts) is kept here per handle.
 * Blocking calls wait forever unless SO_RCVTIMEO / SO_SNDTIMEO say
 * otherwise; non-blocking ones fail with EWOULDBLOCK or EINPROGRESS.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glaze/net.h>
#include <glaze/sys.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_FDS     64
#define RESOLVE_MS  10000

struct fd_state {
    bool used, nonblock;
    int type;                   /* SOCK_STREAM or SOCK_DGRAM */
    int rcvtimeo, sndtimeo;     /* ms, 0 = none */
};

static struct fd_state fds[MAX_FDS];

/* Sets errno from a GZ_* error; returns -1. */
static int set_err(int gz)
{
    switch (gz) {
    case GZ_ETIMEDOUT:      errno = EWOULDBLOCK; break;
    case GZ_EINPROGRESS:    errno = EINPROGRESS; break;
    case GZ_ENETDOWN:       errno = ENETDOWN; break;
    case GZ_ECONNREFUSED:   errno = ECONNREFUSED; break;
    case GZ_ECONNRESET:     errno = ECONNRESET; break;
    case GZ_ENOTCONN:       errno = ENOTCONN; break;
    case GZ_EHOSTUNREACH:   errno = EHOSTUNREACH; break;
    case GZ_EINUSE:         errno = EADDRINUSE; break;
    case GZ_EBUSY:          errno = EBUSY; break;
    case GZ_ENOMEM:         errno = ENOMEM; break;
    case GZ_EFULL:          errno = EMFILE; break;
    case GZ_EIO:            errno = EIO; break;
    case GZ_EPERM:          errno = EACCES; break;
    default:                errno = EINVAL; break;
    }
    return -1;
}

static struct fd_state *fd_get(int fd)
{
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].used) {
        errno = EBADF;
        return NULL;
    }
    return &fds[fd];
}

static int ms_of(const struct timeval *tv)
{
    long ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;

    return ms > 0x7FFFFFFF / 2 ? 0x7FFFFFFF / 2 : (int)ms;
}

static bool to_sin(const struct sockaddr *sa, socklen_t len, uint32_t *addr, uint16_t *port)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

    if (!sa || len < sizeof(*sin) || sin->sin_family != AF_INET)
        return false;
    *addr = sin->sin_addr.s_addr;
    *port = ntohs(sin->sin_port);
    return true;
}

static void from_sin(struct sockaddr *sa, socklen_t *len, uint32_t addr, uint16_t port)
{
    struct sockaddr_in sin;

    if (!sa || !len)
        return;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = addr;
    memcpy(sa, &sin, *len < sizeof(sin) ? *len : sizeof(sin));
    *len = sizeof(sin);
}

/* ---- sockets ------------------------------------------------------------------ */

int socket(int domain, int type, int protocol)
{
    bool nonblock = type & SOCK_NONBLOCK;

    type &= ~SOCK_NONBLOCK;
    if (domain != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    int h = net_socket(type == SOCK_STREAM ? NET_TCP : NET_UDP);
    if (h < 0)
        return set_err(h);
    if (h >= MAX_FDS) {
        net_close(h);
        errno = EMFILE;
        return -1;
    }
    fds[h] = (struct fd_state){ true, nonblock, type, 0, 0 };
    return h;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    struct fd_state *f = fd_get(fd);
    uint32_t a;
    uint16_t port;

    if (!f)
        return -1;
    if (!to_sin(addr, len, &a, &port)) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    int rv = net_connect(fd, a, port, f->nonblock ? 0 : f->sndtimeo ? f->sndtimeo : -1);
    if (rv == GZ_ETIMEDOUT && !f->nonblock) {
        errno = ETIMEDOUT;
        return -1;
    }
    return rv < 0 ? set_err(rv) : 0;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    uint32_t a;
    uint16_t port;

    if (!fd_get(fd))
        return -1;
    if (!to_sin(addr, len, &a, &port)) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (a != INADDR_ANY) {
        errno = EADDRNOTAVAIL;      /* one interface: any address only */
        return -1;
    }
    int rv = net_bind(fd, port);
    return rv < 0 ? set_err(rv) : 0;
}

int listen(int fd, int backlog)
{
    if (!fd_get(fd))
        return -1;
    int rv = net_listen(fd);
    return rv < 0 ? set_err(rv) : 0;
}

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    struct fd_state *f = fd_get(fd);
    uint32_t a;
    uint16_t port;

    if (!f)
        return -1;
    int h = net_accept(fd, &a, &port, f->nonblock ? 0 : f->rcvtimeo ? f->rcvtimeo : -1);
    if (h < 0)
        return set_err(h);
    if (h >= MAX_FDS) {
        net_close(h);
        errno = EMFILE;
        return -1;
    }
    fds[h] = (struct fd_state){ true, false, SOCK_STREAM, 0, 0 };
    from_sin(addr, len, a, port);
    return h;
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    struct fd_state *f = fd_get(fd);

    if (!f)
        return -1;
    bool dontwait = f->nonblock || (flags & MSG_DONTWAIT);
    int rv = net_send_flags(fd, buf, (uint32_t)len, dontwait ? NET_MSG_DONTWAIT : 0);
    return rv < 0 ? set_err(rv) : rv;
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    struct fd_state *f = fd_get(fd);

    if (!f)
        return -1;
    bool dontwait = f->nonblock || (flags & MSG_DONTWAIT);
    int rv = net_recv_flags(fd, buf, (uint32_t)len, dontwait ? 0 : f->rcvtimeo ? f->rcvtimeo : -1,
                            flags & MSG_PEEK ? NET_MSG_PEEK : 0);
    return rv < 0 ? set_err(rv) : rv;
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
    uint32_t a;
    uint16_t port;

    if (!to)
        return send(fd, buf, len, flags);
    if (!fd_get(fd))
        return -1;
    if (!to_sin(to, tolen, &a, &port)) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (len > NET_IO_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    int rv = net_sendto(fd, buf, (uint32_t)len, a, port);
    return rv < 0 ? set_err(rv) : rv;
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
    struct fd_state *f = fd_get(fd);
    uint32_t a;
    uint16_t port;

    if (!f)
        return -1;
    if (f->type == SOCK_STREAM) {
        ssize_t n = recv(fd, buf, len, flags);
        if (n >= 0 && from && fromlen && net_name(fd, true, &a, &port) == 0)
            from_sin(from, fromlen, a, port);
        return n;
    }
    bool dontwait = f->nonblock || (flags & MSG_DONTWAIT);
    int rv = net_recvfrom(fd, buf, (uint32_t)len, &a, &port, dontwait ? 0 : f->rcvtimeo ? f->rcvtimeo : -1);
    if (rv < 0)
        return set_err(rv);
    from_sin(from, fromlen, a, port);
    return rv;
}

int shutdown(int fd, int how)
{
    if (!fd_get(fd))
        return -1;
    int rv = net_shutdown(fd, how == SHUT_RD ? NET_SHUT_RD : how == SHUT_WR ? NET_SHUT_WR : NET_SHUT_RDWR);
    return rv < 0 ? set_err(rv) : 0;
}

int setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
    struct fd_state *f = fd_get(fd);
    int opt = 0;

    if (!f)
        return -1;
    if (!val || len < sizeof(int)) {
        errno = EINVAL;
        return -1;
    }
    int v = *(const int *)val;
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) {
        if (len < sizeof(struct timeval)) {
            errno = EINVAL;
            return -1;
        }
        *(name == SO_RCVTIMEO ? &f->rcvtimeo : &f->sndtimeo) = ms_of(val);
        return 0;
    }
    if (level == SOL_SOCKET && name == SO_REUSEADDR)
        opt = NET_OPT_REUSEADDR;
    else if (level == SOL_SOCKET && name == SO_KEEPALIVE)
        opt = NET_OPT_KEEPALIVE;
    else if (level == IPPROTO_TCP && name == TCP_NODELAY)
        opt = NET_OPT_NODELAY;
    if (!opt) {
        errno = ENOPROTOOPT;
        return -1;
    }
    int rv = net_sockopt(fd, opt, v);
    return rv < 0 ? set_err(rv) : 0;
}

int getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
    struct fd_state *f = fd_get(fd);

    if (!f)
        return -1;
    if (!val || !len || *len < sizeof(int)) {
        errno = EINVAL;
        return -1;
    }
    if (level == SOL_SOCKET && name == SO_ERROR) {
        int rv = net_sockopt(fd, NET_OPT_ERROR, 0);
        int saved = errno;
        *(int *)val = rv == GZ_ETIMEDOUT ? ETIMEDOUT : rv < 0 ? (set_err(rv), errno) : 0;
        errno = saved;
        *len = sizeof(int);
        return 0;
    }
    if (level == SOL_SOCKET && name == SO_TYPE) {
        *(int *)val = f->type;
        *len = sizeof(int);
        return 0;
    }
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO) && *len >= sizeof(struct timeval)) {
        int ms = name == SO_RCVTIMEO ? f->rcvtimeo : f->sndtimeo;
        struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
        memcpy(val, &tv, sizeof(tv));
        *len = sizeof(tv);
        return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    uint32_t a;
    uint16_t port;

    if (!fd_get(fd))
        return -1;
    int rv = net_name(fd, true, &a, &port);
    if (rv < 0)
        return set_err(rv);
    from_sin(addr, len, a, port);
    return 0;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    uint32_t a;
    uint16_t port;

    if (!fd_get(fd))
        return -1;
    int rv = net_name(fd, false, &a, &port);
    if (rv < 0)
        return set_err(rv);
    from_sin(addr, len, a, port);
    return 0;
}

int fcntl(int fd, int cmd, ...)
{
    struct fd_state *f = fd_get(fd);
    va_list ap;

    if (!f)
        return -1;
    if (cmd == F_GETFL)
        return O_RDWR | (f->nonblock ? O_NONBLOCK : 0);
    if (cmd == F_SETFL) {
        va_start(ap, cmd);
        f->nonblock = (va_arg(ap, int) & O_NONBLOCK) != 0;
        va_end(ap);
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int close(int fd)
{
    if (!fd_get(fd))
        return -1;
    fds[fd].used = false;
    int rv = net_close(fd);
    return rv < 0 ? set_err(rv) : 0;
}

ssize_t read(int fd, void *buf, size_t len)
{
    return recv(fd, buf, len, 0);
}

ssize_t write(int fd, const void *buf, size_t len)
{
    return send(fd, buf, len, 0);
}

int poll(struct pollfd *pfds, nfds_t n, int timeout_ms)
{
    struct net_pollfd q[NET_POLL_MAX];
    int map[NET_POLL_MAX], nq = 0, bad = 0;

    for (nfds_t i = 0; i < n; i++) {
        pfds[i].revents = 0;
        if (pfds[i].fd < 0)
            continue;
        if (pfds[i].fd >= MAX_FDS || !fds[pfds[i].fd].used) {
            pfds[i].revents = POLLNVAL;
            bad++;
            continue;
        }
        if (nq == NET_POLL_MAX) {
            errno = EINVAL;
            return -1;
        }
        q[nq] = (struct net_pollfd){ pfds[i].fd, pfds[i].events, 0 };
        map[nq++] = (int)i;
    }
    if (!nq) {
        if (!bad && timeout_ms > 0)
            gz_sleep_ms((uint32_t)timeout_ms);
        return bad;
    }
    int rv = net_poll(q, nq, bad ? 0 : timeout_ms);
    if (rv < 0)
        return set_err(rv);
    for (int i = 0; i < nq; i++)
        pfds[map[i]].revents = q[i].revents;
    return rv + bad;
}

/* ---- addresses and names ---------------------------------------------------------- */

static int parse_ipv4(const char *s, uint8_t out[4])
{
    for (int i = 0; i < 4; i++) {
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) {
            v = v * 10 + (*s++ - '0');
            digits++;
        }
        if (!digits || v > 255 || (i < 3 && *s++ != '.'))
            return 0;
        out[i] = (uint8_t)v;
    }
    return *s == 0;
}

int inet_pton(int af, const char *src, void *dst)
{
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return parse_ipv4(src, dst);
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    const uint8_t *a = src;

    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    if ((socklen_t)snprintf(dst, size, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]) >= size) {
        errno = ENOSPC;
        return NULL;
    }
    return dst;
}

struct ai_block {
    struct addrinfo ai;
    struct sockaddr_in sin;
};

static int service_port(const char *service, unsigned long *port)
{
    static const struct {
        const char *name;
        uint16_t port;
    } known[] = { { "http", 80 }, { "https", 443 }, { "ftp", 21 }, { "domain", 53 }, { "ntp", 123 } };
    char *end;

    if (!service)
        return 0;
    *port = strtoul(service, &end, 10);
    if (!*end && *port <= 65535)
        return 0;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (!strcmp(service, known[i].name)) {
            *port = known[i].port;
            return 0;
        }
    }
    return EAI_SERVICE;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    uint8_t ip[4] = { 0 };
    unsigned long port = 0;

    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
        return EAI_FAMILY;
    if (hints && hints->ai_socktype && hints->ai_socktype != SOCK_STREAM && hints->ai_socktype != SOCK_DGRAM)
        return EAI_SOCKTYPE;
    int e = service_port(service, &port);
    if (e)
        return e;
    if (node && !parse_ipv4(node, ip)) {
        if (hints && (hints->ai_flags & AI_NUMERICHOST))
            return EAI_NONAME;
        uint32_t a;
        int rv = net_resolve(node, &a, RESOLVE_MS);
        if (rv == GZ_ETIMEDOUT)
            return EAI_AGAIN;
        if (rv == GZ_ENETDOWN) {
            errno = ENETDOWN;
            return EAI_SYSTEM;
        }
        if (rv < 0)
            return EAI_NONAME;
        memcpy(ip, &a, 4);
    } else if (!node && !(hints && (hints->ai_flags & AI_PASSIVE))) {
        memcpy(ip, "\x7f\0\0\x01", 4);
    }

    struct ai_block *b = calloc(1, sizeof(*b));
    if (!b)
        return EAI_MEMORY;
    b->sin.sin_family = AF_INET;
    b->sin.sin_port = htons((uint16_t)port);
    memcpy(&b->sin.sin_addr, ip, 4);
    b->ai.ai_family = AF_INET;
    b->ai.ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    b->ai.ai_protocol = b->ai.ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    b->ai.ai_addrlen = sizeof(b->sin);
    b->ai.ai_addr = (struct sockaddr *)&b->sin;
    *res = &b->ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res);              /* the addrinfo starts its ai_block */
        res = next;
    }
}

const char *gai_strerror(int err)
{
    switch (err) {
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
    case EAI_FAMILY: return "Address family not supported";
    case EAI_SOCKTYPE: return "Socket type not supported";
    case EAI_SERVICE: return "Service not supported";
    case EAI_MEMORY: return "Out of memory";
    case EAI_NONAME: return "Name or service not known";
    case EAI_SYSTEM: return "System error";
    default: return "Unknown error";
    }
}
