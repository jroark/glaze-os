/* Network client: requests to the network server (see glaze/net.h). */

#include <glaze/net.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <stdio.h>
#include <string.h>

static int port, session = -1;
static uint8_t *buf;

/* One call at a time per process: the shared buffer is the session's. */
static struct gz_mutex session_lock = GZ_MUTEX_INIT;

static void release(struct gz_mutex **m)
{
    gz_mutex_unlock(*m);
}

#define SESSION_LOCKED() \
    struct gz_mutex *held_ __attribute__((cleanup(release))) = (gz_mutex_lock(&session_lock), &session_lock)

/* Connects on first use, waiting a little for the server after boot. */
static int connect(void)
{
    struct gz_msg m, r;

    if (session >= 0)
        return 0;
    for (int tries = 0; (port = gz_port_lookup(NET_PORT_NAME)) <= 0; tries++) {
        if (tries == 100)
            return GZ_ENETDOWN;
        gz_sleep_ms(50);
    }
    memset(&m, 0, sizeof(m));
    m.w[0] = NET_HELLO;
    m.w[1] = (uint32_t)gz_getpid();
    if (gz_send(port, &m, &r) < 0 || (int)r.w[0] < 0)
        return r.w[0] ? (int)r.w[0] : GZ_EIO;
    buf = gz_shm_map((int)r.w[2]);
    if (!buf)
        return GZ_ENOMEM;
    session = (int)r.w[1];
    return 0;
}

static int call(uint32_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t d, struct gz_msg *reply)
{
    struct gz_msg m, r;
    int rv = connect();

    if (reply)
        memset(reply, 0, sizeof(*reply));
    if (rv)
        return rv;
    memset(&m, 0, sizeof(m));
    m.w[0] = op;
    m.w[1] = (uint32_t)session;
    m.w[2] = a;
    m.w[3] = b;
    m.w[4] = c;
    m.w[5] = d;
    if (gz_send(port, &m, &r) < 0)
        return GZ_EIO;
    if (reply)
        *reply = r;
    return (int)r.w[0];
}

int net_status(struct net_status *st)
{
    struct gz_msg m = {0}, r;
    int peer;

    /* A status read must not wait behind this process's DNS or NTP calls.
     * Return the snapshot in the reply, without a shared-buffer session. */
    _Static_assert(sizeof(*st) <= sizeof(r.w) - sizeof(r.w[0]), "status must fit in reply");
    for (int tries = 0; (peer = gz_port_lookup(NET_PORT_NAME)) <= 0; tries++) {
        if (tries == 100)
            return GZ_ENETDOWN;
        gz_sleep_ms(50);
    }
    m.w[0] = NET_STATUS;
    m.w[1] = (uint32_t)-1;
    if (gz_send(peer, &m, &r) < 0)
        return GZ_EIO;
    if ((int)r.w[0] < 0)
        return (int)r.w[0];
    memcpy(st, &r.w[1], sizeof(*st));
    return 0;
}

int net_wait_up(int timeout_ms)
{
    struct net_status st;
    uint32_t start = gz_time_ms();

    for (;;) {
        if (net_status(&st) == 0 && (st.state == NET_UP || st.state == NET_UP_STATIC))
            return 0;
        if (st.state == NET_NO_CARD || (int)(gz_time_ms() - start) >= timeout_ms)
            return GZ_ENETDOWN;
        gz_sleep_ms(100);
    }
}

int net_resolve(const char *host, uint32_t *addr, int timeout_ms)
{
    SESSION_LOCKED();
    struct gz_msg r;
    size_t n = strlen(host);
    int rv = connect();

    if (rv)
        return rv;
    if (n >= NET_HOST_MAX)
        return GZ_EINVAL;
    memcpy(buf, host, n + 1);
    rv = call(NET_RESOLVE, (uint32_t)timeout_ms, 0, 0, 0, &r);
    if (rv == 0)
        *addr = r.w[1];
    return rv;
}

int net_socket(int type)
{
    SESSION_LOCKED();
    return call(NET_SOCKET, (uint32_t)type, 0, 0, 0, NULL);
}

int net_connect(int s, uint32_t addr, uint16_t port_, int timeout_ms)
{
    SESSION_LOCKED();
    return call(NET_CONNECT, (uint32_t)s, addr, port_, (uint32_t)timeout_ms, NULL);
}

int net_bind(int s, uint16_t port_)
{
    SESSION_LOCKED();
    return call(NET_BIND, (uint32_t)s, port_, 0, 0, NULL);
}

int net_listen(int s)
{
    SESSION_LOCKED();
    return call(NET_LISTEN, (uint32_t)s, 0, 0, 0, NULL);
}

int net_accept(int s, uint32_t *addr, uint16_t *port_, int timeout_ms)
{
    SESSION_LOCKED();
    struct gz_msg r;
    int rv = call(NET_ACCEPT, (uint32_t)s, (uint32_t)timeout_ms, 0, 0, &r);

    if (rv >= 0) {
        if (addr)
            *addr = r.w[1];
        if (port_)
            *port_ = (uint16_t)r.w[2];
    }
    return rv;
}

int net_send_flags(int s, const void *src, uint32_t len, int flags)
{
    SESSION_LOCKED();
    uint32_t done = 0;
    int rv = connect();

    if (rv)
        return rv;
    while (done < len) {
        uint32_t n = MIN(len - done, NET_IO_MAX);
        memcpy(buf, (const uint8_t *)src + done, n);
        rv = call(NET_SEND, (uint32_t)s, n, (uint32_t)flags, 0, NULL);
        if (rv < 0)
            return done ? (int)done : rv;
        done += (uint32_t)rv;
        if ((uint32_t)rv < n)
            break;              /* NET_MSG_DONTWAIT: the rest didn't fit */
    }
    return (int)done;
}

int net_send(int s, const void *src, uint32_t len)
{
    return net_send_flags(s, src, len, 0);
}

int net_recv_flags(int s, void *dst, uint32_t len, int timeout_ms, int flags)
{
    SESSION_LOCKED();
    int rv = call(NET_RECV, (uint32_t)s, MIN(len, NET_IO_MAX), (uint32_t)timeout_ms, (uint32_t)flags, NULL);

    if (rv > 0)
        memcpy(dst, buf, (size_t)rv);
    return rv;
}

int net_recv(int s, void *dst, uint32_t len, int timeout_ms)
{
    return net_recv_flags(s, dst, len, timeout_ms, 0);
}

int net_sendto(int s, const void *src, uint32_t len, uint32_t addr, uint16_t port_)
{
    SESSION_LOCKED();
    int rv = connect();

    if (rv)
        return rv;
    if (len > NET_IO_MAX)
        return GZ_EINVAL;
    memcpy(buf, src, len);
    return call(NET_SENDTO, (uint32_t)s, len, addr, port_, NULL);
}

int net_recvfrom(int s, void *dst, uint32_t len, uint32_t *addr, uint16_t *port_, int timeout_ms)
{
    SESSION_LOCKED();
    struct gz_msg r;
    int rv = call(NET_RECVFROM, (uint32_t)s, MIN(len, NET_IO_MAX), (uint32_t)timeout_ms, 0, &r);

    if (rv >= 0) {
        memcpy(dst, buf, (size_t)rv);
        if (addr)
            *addr = r.w[1];
        if (port_)
            *port_ = (uint16_t)r.w[2];
    }
    return rv;
}

int net_close(int s)
{
    SESSION_LOCKED();
    return call(NET_CLOSE, (uint32_t)s, 0, 0, 0, NULL);
}

int net_ping(uint32_t addr, int timeout_ms)
{
    SESSION_LOCKED();
    return call(NET_PING, addr, (uint32_t)timeout_ms, 0, 0, NULL);
}

int net_poll(struct net_pollfd *fds, int n, int timeout_ms)
{
    SESSION_LOCKED();
    int rv = connect();

    if (rv)
        return rv;
    if (n < 0 || n > NET_POLL_MAX)
        return GZ_EINVAL;
    memcpy(buf, fds, sizeof(*fds) * (size_t)n);
    rv = call(NET_POLL, (uint32_t)n, (uint32_t)timeout_ms, 0, 0, NULL);
    if (rv >= 0)
        memcpy(fds, buf, sizeof(*fds) * (size_t)n);
    return rv;
}

int net_sockopt(int s, int opt, int value)
{
    SESSION_LOCKED();
    return call(NET_SOCKOPT, (uint32_t)s, (uint32_t)opt, (uint32_t)value, 0, NULL);
}

int net_shutdown(int s, int how)
{
    SESSION_LOCKED();
    return call(NET_SHUTDOWN, (uint32_t)s, (uint32_t)how, 0, 0, NULL);
}

int net_name(int s, bool peer, uint32_t *addr, uint16_t *port_)
{
    SESSION_LOCKED();
    struct gz_msg r;
    int rv = call(NET_NAME, (uint32_t)s, peer, 0, 0, &r);

    if (rv == 0) {
        if (addr)
            *addr = r.w[1];
        if (port_)
            *port_ = (uint16_t)r.w[2];
    }
    return rv;
}

char *net_ntoa(uint32_t addr, char *out)
{
    snprintf(out, 16, "%u.%u.%u.%u", addr & 0xFF, (addr >> 8) & 0xFF,
             (addr >> 16) & 0xFF, addr >> 24);
    return out;
}

int net_aton(const char *s, uint32_t *addr)
{
    uint32_t v = 0;

    for (int i = 0; i < 4; i++) {
        uint32_t part = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) {
            part = part * 10 + (uint32_t)(*s++ - '0');
            digits++;
        }
        if (!digits || part > 255 || *s != (i < 3 ? '.' : '\0'))
            return GZ_EINVAL;
        s++;
        v |= part << (8 * i);
    }
    *addr = v;
    return 0;
}

const char *net_strerror(int err)
{
    switch (err) {
    case GZ_ENETDOWN:       return "Not connected to a network";
    case GZ_ETIMEDOUT:      return "The server didn't respond";
    case GZ_ENOENT:         return "Server not found";
    case GZ_ECONNREFUSED:   return "Connection refused";
    case GZ_ECONNRESET:     return "Connection lost";
    case GZ_EHOSTUNREACH:   return "Server unreachable";
    case GZ_ENOMEM:         return "Out of memory";
    default:                return "Network error";
    }
}
