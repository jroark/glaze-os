/*
 * Network server: lwIP's sockets API behind the protocol in glaze/net.h.
 *
 * lwIP runs in its own tcpip thread (netif.c). This main thread receives
 * requests and the kernel's frame-arrival notifications. Calls that finish
 * at once are handled here; calls that wait go to a pool of worker
 * threads, which make the blocking lwip_* call and answer the sender's
 * token themselves (the kernel lets any thread of the server reply).
 *
 * One mutex guards the session and socket tables. Workers drop it while
 * they block, copying data through a buffer of their own, and only write
 * a session's shared buffer if that session still belongs to the caller.
 * A socket in use by a worker is closed when the worker is done with it;
 * closing first shuts it down, which wakes the worker.
 */

#include <errno.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <string.h>

#include "lwip/api.h"
#include "lwip/sockets.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/ip4.h"
#include "server.h"

#define MAX_SESSIONS    8
#define MAX_SOCKETS     16
#define WORKERS         6
#define MAX_JOBS        32
#define BACKLOG         4
#define PING_ID         0x6C67
#define PING_DATA       32
#define LATER           0x7FFFFFFF  /* handle(): a worker answers */

struct session {
    int pid;
    int shm;
    uint8_t *buf;
};

struct sock {
    bool used;
    bool closing;               /* close when the last worker lets go */
    int session;
    int type;
    int fd;                     /* lwIP socket */
    int busy;                   /* workers using it */
};

struct job {
    int token;
    int pid;                    /* session owner when queued */
    struct gz_msg m;
};

static struct session sessions[MAX_SESSIONS];
static struct sock socks[MAX_SOCKETS];
static struct gz_mutex lock = GZ_MUTEX_INIT;
static struct job jobs[MAX_JOBS];
static int job_head, job_count, job_sem;
static uint16_t ping_seq;

static void answer(int token, int rv, uint32_t w1, uint32_t w2)
{
    struct gz_msg r;

    memset(&r, 0, sizeof(r));
    r.w[0] = (uint32_t)rv;
    r.w[1] = w1;
    r.w[2] = w2;
    gz_reply(token, &r);
}

/* lwIP's errno to the protocol's errors. */
static int err_of(int e)
{
    switch (e) {
    case 0:             return 0;
    case EWOULDBLOCK:
    case ETIMEDOUT:     return GZ_ETIMEDOUT;
    case EINPROGRESS:   return GZ_EINPROGRESS;
    case ECONNREFUSED:  return GZ_ECONNREFUSED;
    case ECONNRESET:
    case ECONNABORTED:
    case EPIPE:         return GZ_ECONNRESET;
    case ENOTCONN:      return GZ_ENOTCONN;
    case EHOSTUNREACH:
    case ENETUNREACH:   return GZ_EHOSTUNREACH;
    case ENOMEM:
    case ENOBUFS:
    case ENFILE:        return GZ_ENOMEM;
    case EADDRINUSE:
    case EISCONN:
    case EALREADY:      return GZ_EINUSE;
    case EIO:           return GZ_EIO;
    default:            return GZ_EINVAL;
    }
}

static int fail(void)
{
    return err_of(errno);
}

static void set_addr(struct sockaddr_in *sa, uint32_t addr, uint16_t port)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_len = sizeof(*sa);
    sa->sin_family = AF_INET;
    sa->sin_port = lwip_htons(port);
    sa->sin_addr.s_addr = addr;
}

/* ---- sockets (table lock held) -------------------------------------------- */

static int sock_index(const struct sock *s)
{
    return (int)(s - socks);
}

static struct sock *sock_get(int sess, uint32_t h)
{
    if (h >= MAX_SOCKETS || !socks[h].used || socks[h].closing || socks[h].session != sess)
        return NULL;
    return &socks[h];
}

static struct sock *sock_alloc(int sess, int type, int fd)
{
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!socks[i].used) {
            struct sock *s = &socks[i];
            memset(s, 0, sizeof(*s));
            s->used = true;
            s->session = sess;
            s->type = type;
            s->fd = fd;
            return s;
        }
    }
    return NULL;
}

static void sock_close(struct sock *s)
{
    if (s->busy) {
        s->closing = true;
        lwip_shutdown(s->fd, SHUT_RDWR);    /* wakes the worker */
        return;
    }
    lwip_close(s->fd);
    s->used = false;
}

/* A worker is done with s. */
static void sock_release(struct sock *s)
{
    if (--s->busy == 0 && s->closing) {
        lwip_close(s->fd);
        s->used = false;
    }
}

static void close_session(int sess)
{
    for (int i = 0; i < MAX_SOCKETS; i++)
        if (socks[i].used && !socks[i].closing && socks[i].session == sess)
            sock_close(&socks[i]);
}

static int op_hello(int pid, struct gz_msg *r)
{
    int pick = -1;

    /* Reuse this process's session, else a free one, else a dead one's. */
    for (int i = 0; i < MAX_SESSIONS && pick < 0; i++)
        if (sessions[i].pid == pid)
            pick = i;
    for (int i = 0; i < MAX_SESSIONS && pick < 0; i++)
        if (!sessions[i].pid || !gz_proc_alive(sessions[i].pid))
            pick = i;
    if (pick < 0)
        return GZ_EFULL;
    struct session *s = &sessions[pick];
    close_session(pick);
    if (!s->buf) {
        s->shm = gz_shm_create(NET_BUF_SIZE);
        s->buf = s->shm > 0 ? gz_shm_map(s->shm) : NULL;
        if (!s->buf)
            return GZ_ENOMEM;
    }
    s->pid = pid;
    r->w[1] = (uint32_t)pick;
    r->w[2] = (uint32_t)s->shm;
    return 0;
}

/* Close what exited processes left open. */
static void reap_sessions(void)
{
    gz_mutex_lock(&lock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].pid && !gz_proc_alive(sessions[i].pid)) {
            close_session(i);
            sessions[i].pid = 0;
        }
    }
    gz_mutex_unlock(&lock);
}

/* ---- calls that finish at once (table lock held) ---------------------------- */

static int op_socket(int sess, uint32_t type)
{
    if (type != NET_TCP && type != NET_UDP)
        return GZ_EINVAL;
    int fd = lwip_socket(AF_INET, type == NET_TCP ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (fd < 0)
        return fail();
    struct sock *s = sock_alloc(sess, (int)type, fd);
    if (!s) {
        lwip_close(fd);
        return GZ_EFULL;
    }
    return sock_index(s);
}

static int op_bind(struct sock *s, uint32_t port)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);

    if (s->type == NET_TCP) {
        int one = 1;
        lwip_setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    set_addr(&sa, 0, (uint16_t)port);
    if (lwip_bind(s->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        return fail();
    if (lwip_getsockname(s->fd, (struct sockaddr *)&sa, &len) < 0)
        return fail();
    return lwip_ntohs(sa.sin_port);
}

static int op_sendto(struct sock *s, const struct gz_msg *m, const uint8_t *buf)
{
    struct sockaddr_in sa;
    uint32_t len = m->w[3];

    if (s->type != NET_UDP || len > NET_IO_MAX)
        return GZ_EINVAL;
    set_addr(&sa, m->w[4], (uint16_t)m->w[5]);
    int n = lwip_sendto(s->fd, buf, len, 0, (struct sockaddr *)&sa, sizeof(sa));
    return n < 0 ? fail() : n;
}

static int op_sockopt(struct sock *s, uint32_t opt, uint32_t value)
{
    int v = (int)value, err = 0;
    socklen_t len = sizeof(err);

    switch (opt) {
    case NET_OPT_ERROR:
        if (lwip_getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            return fail();
        return err == ECONNRESET ? GZ_ECONNREFUSED : err_of(err);
    case NET_OPT_NODELAY:
        return lwip_setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) < 0 ? fail() : 0;
    case NET_OPT_KEEPALIVE:
        return lwip_setsockopt(s->fd, SOL_SOCKET, SO_KEEPALIVE, &v, sizeof(v)) < 0 ? fail() : 0;
    case NET_OPT_REUSEADDR:
        return lwip_setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) < 0 ? fail() : 0;
    default:
        return GZ_EINVAL;
    }
}

static int op_name(struct sock *s, bool peer, struct gz_msg *r)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    int rv = peer ? lwip_getpeername(s->fd, (struct sockaddr *)&sa, &len)
                  : lwip_getsockname(s->fd, (struct sockaddr *)&sa, &len);

    if (rv < 0)
        return fail();
    r->w[1] = sa.sin_addr.s_addr;
    r->w[2] = lwip_ntohs(sa.sin_port);
    return 0;
}

/* ---- calls that wait (worker threads) --------------------------------------- */

/* The protocol's poll bits are POSIX's; lwIP numbers its own. */
static short to_lwip_events(int e)
{
    return (short)((e & NET_POLLIN ? POLLIN : 0) | (e & NET_POLLOUT ? POLLOUT : 0));
}

static int16_t from_lwip_events(short e)
{
    return (int16_t)((e & POLLIN ? NET_POLLIN : 0) | (e & POLLOUT ? NET_POLLOUT : 0) |
                     (e & POLLERR ? NET_POLLERR : 0) | (e & POLLHUP ? NET_POLLHUP : 0) |
                     (e & POLLNVAL ? NET_POLLNVAL : 0));
}

/* Waits for events on one socket; 1 ready, 0 timed out, or an error. */
static int wait_for(int fd, short events, int timeout_ms)
{
    struct pollfd p = { fd, events, 0 };
    int n = lwip_poll(&p, 1, timeout_ms);

    return n < 0 ? fail() : n;
}

static int do_connect(int fd, uint32_t addr, uint16_t port, int timeout_ms)
{
    struct sockaddr_in sa;
    int flags = lwip_fcntl(fd, F_GETFL, 0);

    set_addr(&sa, addr, port);
    lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rv = lwip_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ? errno : 0;
    if (rv == EINPROGRESS && timeout_ms != 0) {
        int n = wait_for(fd, POLLOUT, timeout_ms);
        if (n == 0) {
            rv = ETIMEDOUT;
        } else if (n < 0) {
            rv = EIO;
        } else {
            socklen_t len = sizeof(rv);
            lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &rv, &len);
        }
    }
    lwip_fcntl(fd, F_SETFL, flags);
    if (rv == ECONNRESET)
        rv = ECONNREFUSED;      /* a RST answering our SYN */
    return err_of(rv);
}

/* Receive into buf: n bytes, 0 at end of stream, or an error. */
static int do_recv(int fd, uint8_t *buf, uint32_t max, int timeout_ms, int flags, struct sockaddr_in *from)
{
    int f = flags & NET_MSG_PEEK ? MSG_PEEK : 0;

    if (timeout_ms == 0) {
        f |= MSG_DONTWAIT;
    } else if (timeout_ms > 0) {
        int n = wait_for(fd, POLLIN, timeout_ms);
        if (n <= 0)
            return n ? n : GZ_ETIMEDOUT;
    }
    socklen_t len = sizeof(*from);
    int n = from ? lwip_recvfrom(fd, buf, max, f, (struct sockaddr *)from, &len) : lwip_recv(fd, buf, max, f);
    return n < 0 ? fail() : n;
}

static int do_ping(uint32_t addr, int timeout_ms)
{
    uint8_t pkt[sizeof(struct icmp_echo_hdr) + PING_DATA], in[128];
    struct icmp_echo_hdr *echo = (struct icmp_echo_hdr *)pkt;
    struct sockaddr_in sa;
    int fd = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);

    if (fd < 0)
        return fail();
    gz_mutex_lock(&lock);
    uint16_t seq = ++ping_seq;
    gz_mutex_unlock(&lock);
    ICMPH_TYPE_SET(echo, ICMP_ECHO);
    ICMPH_CODE_SET(echo, 0);
    echo->id = PP_HTONS(PING_ID);
    echo->seqno = lwip_htons(seq);
    echo->chksum = 0;
    for (int i = 0; i < PING_DATA; i++)
        pkt[sizeof(*echo) + i] = (uint8_t)i;
    echo->chksum = inet_chksum(pkt, sizeof(pkt));
    set_addr(&sa, addr, 0);
    uint32_t start = gz_time_ms();
    int rv = GZ_ETIMEDOUT;
    if (lwip_sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        rv = fail();
    } else {
        for (;;) {
            int left = timeout_ms < 0 ? -1 : timeout_ms - (int)(gz_time_ms() - start);
            if (timeout_ms >= 0 && left <= 0)
                break;
            if (wait_for(fd, POLLIN, left) <= 0)
                break;
            int n = lwip_recv(fd, in, sizeof(in), 0);
            if (n < (int)sizeof(struct ip_hdr))
                continue;
            int hlen = (in[0] & 0x0F) * 4;     /* raw sockets get the IP header */
            struct icmp_echo_hdr *e = (struct icmp_echo_hdr *)(in + hlen);
            if (n >= hlen + (int)sizeof(*e) && e->type == ICMP_ER && e->id == PP_HTONS(PING_ID) &&
                e->seqno == lwip_htons(seq)) {
                rv = (int)(gz_time_ms() - start);
                break;
            }
        }
    }
    lwip_close(fd);
    return rv;
}

static int do_resolve(const char *name, int timeout_ms, uint32_t *addr)
{
    ip_addr_t a;
    uint32_t start = gz_time_ms();

    /* lwIP's DNS has its own retries; a late answer still times out. */
    if (netconn_gethostbyname(name, &a) != ERR_OK)
        return GZ_ENOENT;
    if (timeout_ms >= 0 && (int)(gz_time_ms() - start) > timeout_ms)
        return GZ_ETIMEDOUT;
    *addr = ip4_addr_get_u32(ip_2_ip4(&a));
    return 0;
}

/* The session's buffer, if it still belongs to the job's caller (lock held). */
static uint8_t *job_buf(const struct job *j)
{
    int sess = (int)j->m.w[1];

    return sessions[sess].pid == j->pid ? sessions[sess].buf : NULL;
}

static void run_job(struct job *j, uint8_t *data)
{
    const struct gz_msg *m = &j->m;
    int sess = (int)m->w[1], rv = 0;
    uint32_t w1 = 0, w2 = 0;
    struct sock *s = NULL;
    int fd = -1;
    uint8_t *buf;

    /* Take what the call needs from the tables and the shared buffer. */
    gz_mutex_lock(&lock);
    buf = job_buf(j);
    switch (m->w[0]) {
    case NET_RESOLVE:
        if (buf) {
            memcpy(data, buf, NET_HOST_MAX);
            data[NET_HOST_MAX - 1] = 0;
        }
        break;
    case NET_POLL: {
        uint32_t n = MIN(m->w[2], NET_POLL_MAX);
        struct net_pollfd *in = (struct net_pollfd *)data;
        struct pollfd *pf = (struct pollfd *)(data + 1024);
        if (buf)
            memcpy(in, buf, n * sizeof(*in));
        for (uint32_t i = 0; i < n; i++) {
            struct sock *ps = buf ? sock_get(sess, (uint32_t)in[i].sock) : NULL;
            pf[i].fd = ps ? ps->fd : -1;
            pf[i].events = to_lwip_events(in[i].events);
            pf[i].revents = 0;
            if (ps)
                ps->busy++;
        }
        break;
    }
    case NET_PING:
        break;
    default:
        s = buf ? sock_get(sess, m->w[2]) : NULL;
        if (s) {
            s->busy++;
            fd = s->fd;
            if (m->w[0] == NET_SEND)
                memcpy(data, buf, MIN(m->w[3], NET_IO_MAX));
        }
        break;
    }
    gz_mutex_unlock(&lock);
    if (!buf) {
        answer(j->token, GZ_EPERM, 0, 0);
        return;
    }

    struct sockaddr_in from;
    switch (m->w[0]) {
    case NET_RESOLVE:
        rv = do_resolve((const char *)data, (int)m->w[2], &w1);
        break;
    case NET_PING:
        rv = do_ping(m->w[2], (int)m->w[3]);
        break;
    case NET_POLL: {
        uint32_t n = MIN(m->w[2], NET_POLL_MAX);
        struct pollfd *pf = (struct pollfd *)(data + 1024);
        struct pollfd live[NET_POLL_MAX];
        int map[NET_POLL_MAX], nl = 0, bad = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (pf[i].fd < 0) {
                pf[i].revents = POLLNVAL;
                bad++;
            } else {
                map[nl] = (int)i;
                live[nl++] = pf[i];
            }
        }
        rv = nl ? lwip_poll(live, (nfds_t)nl, bad ? 0 : (int)m->w[3]) : 0;
        if (!nl && !bad && (int)m->w[3] > 0)
            gz_sleep_ms(m->w[3]);
        if (rv < 0)
            rv = fail();
        else {
            for (int i = 0; i < nl; i++)
                pf[map[i]].revents = live[i].revents;
            rv += bad;
        }
        break;
    }
    case NET_CONNECT:
        rv = s ? do_connect(fd, m->w[3], (uint16_t)m->w[4], (int)m->w[5]) : GZ_EINVAL;
        break;
    case NET_ACCEPT:
        if (!s || s->type != NET_TCP) {
            rv = GZ_EINVAL;
            break;
        }
        rv = wait_for(fd, POLLIN, (int)m->w[3]);
        if (rv == 0)
            rv = GZ_ETIMEDOUT;
        if (rv > 0) {
            socklen_t len = sizeof(from);
            int nfd = lwip_accept(fd, (struct sockaddr *)&from, &len);
            rv = nfd < 0 ? fail() : nfd;
            w1 = from.sin_addr.s_addr;
            w2 = lwip_ntohs(from.sin_port);
        }
        break;
    case NET_SEND:
        if (!s || m->w[3] > NET_IO_MAX) {
            rv = GZ_EINVAL;
            break;
        }
        rv = m->w[3] ? lwip_send(fd, data, m->w[3], m->w[4] & NET_MSG_DONTWAIT ? MSG_DONTWAIT : 0) : 0;
        if (rv < 0)
            rv = fail();
        break;
    case NET_RECV:
        rv = s ? do_recv(fd, data, MIN(m->w[3], NET_IO_MAX), (int)m->w[4], (int)m->w[5], NULL) : GZ_EINVAL;
        break;
    case NET_RECVFROM:
        rv = s && s->type == NET_UDP ? do_recv(fd, data, MIN(m->w[3], NET_IO_MAX), (int)m->w[4], 0, &from)
                                     : GZ_EINVAL;
        if (rv >= 0) {
            w1 = from.sin_addr.s_addr;
            w2 = lwip_ntohs(from.sin_port);
        }
        break;
    default:
        rv = GZ_EINVAL;
        break;
    }

    /* Results back into the tables and the shared buffer. */
    gz_mutex_lock(&lock);
    buf = job_buf(j);
    if (m->w[0] == NET_ACCEPT && rv >= 0) {
        struct sock *c = buf ? sock_alloc(sess, NET_TCP, rv) : NULL;
        if (!c) {
            lwip_close(rv);
            rv = buf ? GZ_EFULL : GZ_EPERM;
        } else {
            rv = sock_index(c);
        }
    }
    if (buf && rv > 0 && (m->w[0] == NET_RECV || m->w[0] == NET_RECVFROM))
        memcpy(buf, data, (size_t)rv);
    if (m->w[0] == NET_POLL) {
        uint32_t n = MIN(m->w[2], NET_POLL_MAX);
        struct net_pollfd *in = (struct net_pollfd *)data;
        struct pollfd *pf = (struct pollfd *)(data + 1024);
        for (uint32_t i = 0; i < n; i++) {
            in[i].revents = from_lwip_events(pf[i].revents);
            if (pf[i].fd >= 0)
                for (int k = 0; k < MAX_SOCKETS; k++)
                    if (socks[k].used && socks[k].fd == pf[i].fd && socks[k].busy) {
                        sock_release(&socks[k]);
                        break;
                    }
        }
        if (buf)
            memcpy(buf, in, n * sizeof(*in));
    }
    if (s)
        sock_release(s);
    gz_mutex_unlock(&lock);
    answer(j->token, rv, w1, w2);
}

static void worker(void *arg)
{
    static uint8_t bufs[WORKERS][NET_IO_MAX + 1024];
    uint8_t *data = bufs[(uintptr_t)arg];

    for (;;) {
        struct job j;
        gz_sem_wait(job_sem, -1);
        gz_mutex_lock(&lock);
        j = jobs[job_head];
        job_head = (job_head + 1) % MAX_JOBS;
        job_count--;
        gz_mutex_unlock(&lock);
        run_job(&j, data);
    }
}

/* ---- requests --------------------------------------------------------------- */

static bool waits(uint32_t op)
{
    switch (op) {
    case NET_CONNECT: case NET_ACCEPT: case NET_SEND: case NET_RECV: case NET_RECVFROM:
    case NET_RESOLVE: case NET_PING: case NET_POLL:
        return true;
    }
    return false;
}

/* A result to reply now, or LATER when a worker will (table lock held). */
static int handle(const struct gz_msg *m, struct gz_msg *r, int token)
{
    uint32_t op = m->w[0];
    int sess = (int)m->w[1];

    if (op == NET_HELLO)
        return op_hello((int)m->w[1], r);
    if (sess < 0 || sess >= MAX_SESSIONS || !sessions[sess].pid)
        return GZ_EPERM;
    if (op == NET_STATUS) {
        net_if_status((struct net_status *)sessions[sess].buf);
        return 0;
    }
    int state = net_if_state();
    if (state != NET_UP && state != NET_UP_STATIC)
        return GZ_ENETDOWN;
    if (waits(op)) {
        if (job_count == MAX_JOBS)
            return GZ_EFULL;
        struct job *j = &jobs[(job_head + job_count++) % MAX_JOBS];
        j->token = token;
        j->pid = sessions[sess].pid;
        j->m = *m;
        gz_sem_post(job_sem);
        return LATER;
    }
    if (op == NET_SOCKET)
        return op_socket(sess, m->w[2]);

    struct sock *s = sock_get(sess, m->w[2]);
    if (!s)
        return GZ_EINVAL;
    switch (op) {
    case NET_CLOSE:
        sock_close(s);
        return 0;
    case NET_BIND:
        return op_bind(s, m->w[3]);
    case NET_LISTEN:
        if (s->type != NET_TCP)
            return GZ_EINVAL;
        return lwip_listen(s->fd, BACKLOG) < 0 ? fail() : 0;
    case NET_SENDTO:
        return op_sendto(s, m, sessions[sess].buf);
    case NET_SOCKOPT:
        return op_sockopt(s, m->w[3], m->w[4]);
    case NET_SHUTDOWN:
        return lwip_shutdown(s->fd, m->w[3] == NET_SHUT_RD ? SHUT_RD : m->w[3] == NET_SHUT_WR ? SHUT_WR
                                                              : SHUT_RDWR) < 0 ? fail() : 0;
    case NET_NAME:
        return op_name(s, m->w[3] != 0, r);
    default:
        return GZ_EINVAL;
    }
}

int main(void)
{
    struct gz_msg m, r;
    int port = gz_port_create();

    if (port < 0 || gz_mutex_init(&lock) < 0 || (job_sem = gz_sem_create(0)) < 0)
        return 1;
    net_if_init(port);
    for (int i = 0; i < WORKERS; i++)
        if (gz_thread_create(worker, (void *)(uintptr_t)i) < 0)
            return 1;
    if (gz_port_register(port, NET_PORT_NAME) < 0)
        return 1;

    uint32_t last_reap = gz_time_ms();
    for (;;) {
        /* Wake at least once a second to reap sessions and check DHCP. */
        int token = gz_recv(port, &m, 1000);

        if (token == GZ_RECV_NOTIFY) {
            net_if_poll();
        } else if (token > 0) {
            reap_sessions();
            memset(&r, 0, sizeof(r));
            gz_mutex_lock(&lock);
            int rv = handle(&m, &r, token);
            gz_mutex_unlock(&lock);
            if (rv != LATER) {
                r.w[0] = (uint32_t)rv;
                gz_reply(token, &r);
            }
        }
        net_if_poll();
        uint32_t now = gz_time_ms();
        if (now - last_reap >= 1000) {
            reap_sessions();
            last_reap = now;
        }
    }
}
