/*
 * Network server: lwIP's raw API behind the protocol in glaze/net.h.
 *
 * One loop does everything: it receives requests and the kernel's
 * frame-arrival notifications, feeds frames to lwIP, and runs lwIP's
 * timers. A call that has to wait is remembered with its sender's token
 * and answered from an lwIP callback or when its deadline passes; the
 * kernel keeps the sender blocked until then.
 */

#include <glaze/sys.h>
#include <string.h>

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/dns.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/ip4.h"
#include "lwip/timeouts.h"
#include "server.h"

#define MAX_SESSIONS    8
#define MAX_SOCKETS     16
#define BACKLOG         4
#define MAX_DGRAMS      4
#define MAX_LOOKUPS     4
#define MAX_PINGS       4
#define PING_ID         0x6C67
#define PING_DATA       32
#define LATER           0x7FFFFFFF  /* dispatch(): the call is answered later */

struct session {
    int pid;
    int shm;
    uint8_t *buf;
};

enum { P_NONE, P_CONNECT, P_ACCEPT, P_SEND, P_RECV, P_RECVFROM };

/* A call waiting for an event. deadline 0 = none. */
struct pending {
    int op;
    int token;
    uint32_t deadline;
    uint32_t len;
};

struct dgram {
    struct pbuf *p;
    uint32_t addr;
    uint16_t port;
};

struct sock {
    bool used;
    int session;
    int type;
    struct tcp_pcb *tcp;
    struct udp_pcb *udp;
    struct pbuf *rx;            /* TCP data not yet read */
    bool fin;                   /* the peer closed */
    int err;                    /* connection failed or reset */
    int backlog[BACKLOG];       /* accepted connections not yet handed out */
    int nbacklog;
    struct dgram dgrams[MAX_DGRAMS];
    int ndgrams;
    struct pending pend;
};

/* DNS lookups and pings wait outside any socket. */
struct waiter {
    bool used;
    int session;
    int token;
    uint32_t deadline;
    uint32_t start;
    uint16_t seq;               /* lookup generation or ping sequence */
};

static struct session sessions[MAX_SESSIONS];
static struct sock socks[MAX_SOCKETS];
static struct waiter lookups[MAX_LOOKUPS], pings[MAX_PINGS];
static struct raw_pcb *ping_pcb;
static uint16_t next_seq;

static uint32_t deadline_of(uint32_t timeout)
{
    if ((int32_t)timeout < 0)
        return 0;
    uint32_t d = gz_time_ms() + timeout;
    return d ? d : 1;
}

static bool expired(uint32_t deadline, uint32_t now)
{
    return deadline && (int32_t)(now - deadline) >= 0;
}

static void answer(int token, int rv, uint32_t w1, uint32_t w2)
{
    struct gz_msg r;

    memset(&r, 0, sizeof(r));
    r.w[0] = (uint32_t)rv;
    r.w[1] = w1;
    r.w[2] = w2;
    gz_reply(token, &r);
}

static int err_of(err_t e)
{
    switch (e) {
    case ERR_OK:        return 0;
    case ERR_MEM:
    case ERR_BUF:       return GZ_ENOMEM;
    case ERR_TIMEOUT:   return GZ_ETIMEDOUT;
    case ERR_RTE:       return GZ_EHOSTUNREACH;
    case ERR_USE:
    case ERR_ISCONN:
    case ERR_ALREADY:   return GZ_EINUSE;
    case ERR_CONN:      return GZ_ENOTCONN;
    case ERR_RST:       return GZ_ECONNRESET;
    case ERR_ABRT:
    case ERR_CLSD:      return GZ_ECONNRESET;
    case ERR_IF:        return GZ_ENETDOWN;
    default:            return GZ_EINVAL;
    }
}

static int sock_index(struct sock *s)
{
    return (int)(s - socks);
}

static struct sock *sock_arg(int sess, uint32_t h)
{
    if (h >= MAX_SOCKETS || !socks[h].used || socks[h].session != sess)
        return NULL;
    return &socks[h];
}

static void finish(struct sock *s, int rv, uint32_t w1, uint32_t w2)
{
    int token = s->pend.token;

    s->pend.op = P_NONE;
    answer(token, rv, w1, w2);
}

/* ---- completing waiting calls ------------------------------------------- */

static void try_recv(struct sock *s)
{
    if (s->pend.op != P_RECV)
        return;
    if (s->rx) {
        uint8_t *buf = sessions[s->session].buf;
        uint16_t n = pbuf_copy_partial(s->rx, buf, (u16_t)MIN(s->pend.len, NET_IO_MAX), 0);
        s->rx = pbuf_free_header(s->rx, n);
        if (s->tcp)
            tcp_recved(s->tcp, n);
        finish(s, n, 0, 0);
    } else if (s->err) {
        finish(s, s->err, 0, 0);
    } else if (s->fin) {
        finish(s, 0, 0, 0);
    }
}

static void try_send(struct sock *s)
{
    if (s->pend.op != P_SEND)
        return;
    if (!s->tcp) {
        finish(s, s->err ? s->err : GZ_ENOTCONN, 0, 0);
        return;
    }
    uint32_t n = MIN(s->pend.len, tcp_sndbuf(s->tcp));
    if (tcp_sndqueuelen(s->tcp) >= TCP_SND_QUEUELEN)
        n = 0;
    if (!n)
        return;                 /* wait for tcp_sent */
    err_t e = tcp_write(s->tcp, sessions[s->session].buf, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (e == ERR_MEM)
        return;
    if (e == ERR_OK)
        tcp_output(s->tcp);
    finish(s, e == ERR_OK ? (int)n : err_of(e), 0, 0);
}

static void try_recvfrom(struct sock *s)
{
    if (s->pend.op != P_RECVFROM || !s->ndgrams)
        return;
    struct dgram d = s->dgrams[0];
    memmove(&s->dgrams[0], &s->dgrams[1], sizeof(d) * (size_t)(s->ndgrams - 1));
    s->ndgrams--;
    uint16_t n = pbuf_copy_partial(d.p, sessions[s->session].buf, (u16_t)MIN(s->pend.len, NET_IO_MAX), 0);
    pbuf_free(d.p);
    finish(s, n, d.addr, d.port);
}

static void sock_free(struct sock *s);

static void try_accept(struct sock *s)
{
    if (s->pend.op != P_ACCEPT || !s->nbacklog)
        return;
    struct sock *c = &socks[s->backlog[0]];
    memmove(&s->backlog[0], &s->backlog[1], sizeof(int) * (size_t)(s->nbacklog - 1));
    s->nbacklog--;
    uint32_t addr = 0, port = 0;
    if (c->tcp) {
        addr = ip4_addr_get_u32(ip_2_ip4(&c->tcp->remote_ip));
        port = c->tcp->remote_port;
    }
    finish(s, sock_index(c), addr, port);
}

/* ---- lwIP callbacks ------------------------------------------------------ */

static void tcp_detach(struct tcp_pcb *pcb)
{
    tcp_arg(pcb, NULL);
    if (pcb->state == LISTEN) {
        tcp_accept(pcb, NULL);
        return;
    }
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
}

static void on_tcp_err(void *arg, err_t e)
{
    struct sock *s = arg;

    if (!s)
        return;
    s->tcp = NULL;              /* lwIP has freed it */
    s->err = e == ERR_RST && s->pend.op == P_CONNECT ? GZ_ECONNREFUSED : err_of(e);
    if (s->pend.op == P_CONNECT || s->pend.op == P_SEND)
        finish(s, s->err, 0, 0);
    try_recv(s);
}

static err_t on_tcp_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t e)
{
    struct sock *s = arg;

    (void)pcb;
    (void)e;
    if (!p) {
        s->fin = true;
    } else if (s->rx) {
        pbuf_cat(s->rx, p);
    } else {
        s->rx = p;
    }
    try_recv(s);
    return ERR_OK;
}

static err_t on_tcp_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb;
    (void)len;
    try_send(arg);
    return ERR_OK;
}

static err_t on_tcp_connected(void *arg, struct tcp_pcb *pcb, err_t e)
{
    struct sock *s = arg;

    (void)pcb;
    if (s->pend.op == P_CONNECT)
        finish(s, err_of(e), 0, 0);
    return ERR_OK;
}

static void tcp_attach(struct sock *s, struct tcp_pcb *pcb)
{
    s->tcp = pcb;
    tcp_arg(pcb, s);
    tcp_recv(pcb, on_tcp_recv);
    tcp_sent(pcb, on_tcp_sent);
    tcp_err(pcb, on_tcp_err);
}

static struct sock *sock_alloc(int sess, int type)
{
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!socks[i].used) {
            struct sock *s = &socks[i];
            memset(s, 0, sizeof(*s));
            s->used = true;
            s->session = sess;
            s->type = type;
            return s;
        }
    }
    return NULL;
}

static err_t on_tcp_accept(void *arg, struct tcp_pcb *pcb, err_t e)
{
    struct sock *l = arg;
    struct sock *c;

    if (e != ERR_OK || !pcb)
        return ERR_VAL;
    if (l->nbacklog == BACKLOG || !(c = sock_alloc(l->session, NET_TCP))) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    tcp_attach(c, pcb);
    l->backlog[l->nbacklog++] = sock_index(c);
    try_accept(l);
    return ERR_OK;
}

static void on_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    struct sock *s = arg;

    (void)pcb;
    if (s->ndgrams == MAX_DGRAMS) {
        pbuf_free(p);
        return;
    }
    s->dgrams[s->ndgrams].p = p;
    s->dgrams[s->ndgrams].addr = ip4_addr_get_u32(ip_2_ip4(addr));
    s->dgrams[s->ndgrams].port = port;
    s->ndgrams++;
    try_recvfrom(s);
}

/* ---- sockets ------------------------------------------------------------- */

static void sock_free(struct sock *s)
{
    if (s->tcp) {
        struct tcp_pcb *pcb = s->tcp;
        tcp_detach(pcb);
        if (tcp_close(pcb) != ERR_OK)
            tcp_abort(pcb);
    }
    if (s->udp)
        udp_remove(s->udp);
    if (s->rx)
        pbuf_free(s->rx);
    for (int i = 0; i < s->ndgrams; i++)
        pbuf_free(s->dgrams[i].p);
    s->used = false;
    for (int i = 0; i < s->nbacklog; i++)
        if (socks[s->backlog[i]].used)
            sock_free(&socks[s->backlog[i]]);
}

static int op_socket(int sess, uint32_t type)
{
    struct sock *s;

    if (type != NET_TCP && type != NET_UDP)
        return GZ_EINVAL;
    if (!(s = sock_alloc(sess, (int)type)))
        return GZ_EFULL;
    if (type == NET_TCP) {
        struct tcp_pcb *pcb = tcp_new();
        if (pcb)
            tcp_attach(s, pcb);
    } else {
        s->udp = udp_new();
        if (s->udp)
            udp_recv(s->udp, on_udp_recv, s);
    }
    if (!s->tcp && !s->udp) {
        s->used = false;
        return GZ_ENOMEM;
    }
    return sock_index(s);
}

static int op_connect(struct sock *s, const struct gz_msg *m, int token)
{
    ip_addr_t addr;

    if (s->type != NET_TCP || !s->tcp || s->tcp->state != CLOSED)
        return s->err ? s->err : GZ_EINUSE;
    ip_addr_set_ip4_u32(&addr, m->w[3]);
    err_t e = tcp_connect(s->tcp, &addr, (u16_t)m->w[4], on_tcp_connected);
    if (e != ERR_OK)
        return err_of(e);
    s->pend = (struct pending){ P_CONNECT, token, deadline_of(m->w[5]), 0 };
    return LATER;
}

static int op_bind(struct sock *s, uint32_t port)
{
    err_t e;

    if (s->tcp) {
        e = tcp_bind(s->tcp, IP_ANY_TYPE, (u16_t)port);
        return e ? err_of(e) : s->tcp->local_port;
    }
    if (s->udp) {
        e = udp_bind(s->udp, IP_ANY_TYPE, (u16_t)port);
        return e ? err_of(e) : s->udp->local_port;
    }
    return GZ_EINVAL;
}

static int op_listen(struct sock *s)
{
    if (!s->tcp || s->tcp->state != CLOSED || !s->tcp->local_port)
        return GZ_EINVAL;
    struct tcp_pcb *l = tcp_listen_with_backlog(s->tcp, BACKLOG);
    if (!l)
        return GZ_ENOMEM;
    s->tcp = l;                 /* tcp_listen freed the original pcb */
    tcp_arg(l, s);
    tcp_accept(l, on_tcp_accept);
    return 0;
}

static int op_sendto(struct sock *s, const struct gz_msg *m)
{
    uint32_t len = m->w[3];
    ip_addr_t addr;

    if (!s->udp || len > NET_IO_MAX)
        return GZ_EINVAL;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p)
        return GZ_ENOMEM;
    pbuf_take(p, sessions[s->session].buf, (u16_t)len);
    ip_addr_set_ip4_u32(&addr, m->w[4]);
    err_t e = udp_sendto(s->udp, p, &addr, (u16_t)m->w[5]);
    pbuf_free(p);
    return e ? err_of(e) : (int)len;
}

/* ---- DNS lookups and pings ------------------------------------------------ */

static struct waiter *waiter_alloc(struct waiter *w, int n, int sess, int token, uint32_t timeout)
{
    for (int i = 0; i < n; i++) {
        if (!w[i].used) {
            w[i] = (struct waiter){ true, sess, token, deadline_of(timeout), gz_time_ms(), ++next_seq };
            return &w[i];
        }
    }
    return NULL;
}

/* lwIP's callback argument packs the slot and its generation. */
static void on_dns(const char *name, const ip_addr_t *addr, void *arg)
{
    uint32_t v = (uint32_t)(uintptr_t)arg;
    struct waiter *w = &lookups[v & 0xFF];

    (void)name;
    if (!w->used || w->seq != (uint16_t)(v >> 8))
        return;
    w->used = false;
    if (addr)
        answer(w->token, 0, ip4_addr_get_u32(ip_2_ip4(addr)), 0);
    else
        answer(w->token, GZ_ENOENT, 0, 0);
}

static int op_resolve(int sess, const struct gz_msg *m, int token)
{
    char *name = (char *)sessions[sess].buf;
    ip_addr_t addr;

    name[NET_HOST_MAX - 1] = 0;
    if (!name[0])
        return GZ_EINVAL;
    struct waiter *w = waiter_alloc(lookups, MAX_LOOKUPS, sess, token, m->w[2]);
    if (!w)
        return GZ_EFULL;
    uint32_t arg = (uint32_t)(w - lookups) | (uint32_t)w->seq << 8;
    err_t e = dns_gethostbyname(name, &addr, on_dns, (void *)(uintptr_t)arg);
    if (e == ERR_INPROGRESS)
        return LATER;
    w->used = false;
    if (e != ERR_OK)
        return GZ_ENOENT;
    answer(token, 0, ip4_addr_get_u32(ip_2_ip4(&addr)), 0);
    return LATER;
}

static u8_t on_icmp(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    struct icmp_echo_hdr echo;
    uint8_t vhl;

    (void)arg;
    (void)pcb;
    (void)addr;
    if (pbuf_copy_partial(p, &vhl, 1, 0) != 1)
        return 0;
    uint16_t hlen = (uint16_t)((vhl & 0x0F) * 4);
    if (pbuf_copy_partial(p, &echo, sizeof(echo), hlen) != sizeof(echo) ||
        echo.type != ICMP_ER || echo.id != PP_HTONS(PING_ID))
        return 0;
    for (int i = 0; i < MAX_PINGS; i++) {
        struct waiter *w = &pings[i];
        if (w->used && echo.seqno == lwip_htons(w->seq)) {
            w->used = false;
            answer(w->token, (int)(gz_time_ms() - w->start), 0, 0);
        }
    }
    pbuf_free(p);
    return 1;
}

static int op_ping(int sess, const struct gz_msg *m, int token)
{
    ip_addr_t addr;

    if (!ping_pcb)
        return GZ_ENOMEM;
    struct waiter *w = waiter_alloc(pings, MAX_PINGS, sess, token, m->w[3]);
    if (!w)
        return GZ_EFULL;
    struct pbuf *p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr) + PING_DATA, PBUF_RAM);
    if (!p) {
        w->used = false;
        return GZ_ENOMEM;
    }
    struct icmp_echo_hdr *echo = p->payload;
    ICMPH_TYPE_SET(echo, ICMP_ECHO);
    ICMPH_CODE_SET(echo, 0);
    echo->id = PP_HTONS(PING_ID);
    echo->seqno = lwip_htons(w->seq);
    echo->chksum = 0;
    for (int i = 0; i < PING_DATA; i++)
        ((uint8_t *)(echo + 1))[i] = (uint8_t)i;
    echo->chksum = inet_chksum(echo, p->len);
    ip_addr_set_ip4_u32(&addr, m->w[2]);
    err_t e = raw_sendto(ping_pcb, p, &addr);
    pbuf_free(p);
    if (e != ERR_OK) {
        w->used = false;
        return err_of(e);
    }
    return LATER;
}

/* ---- sessions ------------------------------------------------------------ */

static void close_session(int sess)
{
    for (int i = 0; i < MAX_SOCKETS; i++)
        if (socks[i].used && socks[i].session == sess)
            sock_free(&socks[i]);
    for (int i = 0; i < MAX_LOOKUPS; i++)
        if (lookups[i].session == sess)
            lookups[i].used = false;
    for (int i = 0; i < MAX_PINGS; i++)
        if (pings[i].session == sess)
            pings[i].used = false;
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
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].pid && !gz_proc_alive(sessions[i].pid)) {
            close_session(i);
            sessions[i].pid = 0;
        }
    }
}

/* Returns a result to reply now, or LATER when the call is answered later. */
static int dispatch(const struct gz_msg *m, struct gz_msg *r, int token)
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
    switch (op) {
    case NET_RESOLVE:   return op_resolve(sess, m, token);
    case NET_SOCKET:    return op_socket(sess, m->w[2]);
    case NET_PING:      return op_ping(sess, m, token);
    }

    struct sock *s = sock_arg(sess, m->w[2]);
    if (!s)
        return GZ_EINVAL;
    if (op == NET_CLOSE) {
        if (s->pend.op != P_NONE)
            finish(s, GZ_ECONNRESET, 0, 0);
        sock_free(s);
        return 0;
    }
    if (s->pend.op != P_NONE)
        return GZ_EBUSY;
    switch (op) {
    case NET_CONNECT:
        return op_connect(s, m, token);
    case NET_BIND:
        return op_bind(s, m->w[3]);
    case NET_LISTEN:
        return op_listen(s);
    case NET_ACCEPT:
        if (!s->tcp || s->tcp->state != LISTEN)
            return GZ_EINVAL;
        s->pend = (struct pending){ P_ACCEPT, token, deadline_of(m->w[3]), 0 };
        try_accept(s);
        return LATER;
    case NET_SEND:
        if (s->type != NET_TCP || m->w[3] > NET_IO_MAX)
            return GZ_EINVAL;
        if (!m->w[3])
            return 0;
        s->pend = (struct pending){ P_SEND, token, 0, m->w[3] };
        try_send(s);
        return LATER;
    case NET_RECV:
        if (s->type != NET_TCP)
            return GZ_EINVAL;
        s->pend = (struct pending){ P_RECV, token, deadline_of(m->w[4]), m->w[3] };
        try_recv(s);
        return LATER;
    case NET_SENDTO:
        return op_sendto(s, m);
    case NET_RECVFROM:
        if (!s->udp)
            return GZ_EINVAL;
        s->pend = (struct pending){ P_RECVFROM, token, deadline_of(m->w[4]), m->w[3] };
        try_recvfrom(s);
        return LATER;
    default:
        return GZ_EINVAL;
    }
}

static void expire(uint32_t now)
{
    for (int i = 0; i < MAX_SOCKETS; i++) {
        struct sock *s = &socks[i];
        if (!s->used || s->pend.op == P_NONE || !expired(s->pend.deadline, now))
            continue;
        if (s->pend.op == P_CONNECT && s->tcp) {
            /* Give up on the handshake; the socket can't be reused. */
            tcp_detach(s->tcp);
            tcp_abort(s->tcp);
            s->tcp = NULL;
            s->err = GZ_ETIMEDOUT;
        }
        finish(s, GZ_ETIMEDOUT, 0, 0);
    }
    for (int i = 0; i < MAX_LOOKUPS; i++) {
        if (lookups[i].used && expired(lookups[i].deadline, now)) {
            lookups[i].used = false;
            answer(lookups[i].token, GZ_ETIMEDOUT, 0, 0);
        }
    }
    for (int i = 0; i < MAX_PINGS; i++) {
        if (pings[i].used && expired(pings[i].deadline, now)) {
            pings[i].used = false;
            answer(pings[i].token, GZ_ETIMEDOUT, 0, 0);
        }
    }
}

/* How long the loop may sleep: until lwIP's next timer or a deadline. */
static int sleep_ms(void)
{
    uint32_t now = gz_time_ms();
    uint32_t t = sys_timeouts_sleeptime();
    uint32_t d;

#define NEAREST(dl) \
    if (dl) { d = (int32_t)((dl) - now) > 0 ? (dl) - now : 0; t = MIN(t, d); }
    for (int i = 0; i < MAX_SOCKETS; i++)
        if (socks[i].used && socks[i].pend.op != P_NONE)
            NEAREST(socks[i].pend.deadline);
    for (int i = 0; i < MAX_LOOKUPS; i++)
        if (lookups[i].used)
            NEAREST(lookups[i].deadline);
    for (int i = 0; i < MAX_PINGS; i++)
        if (pings[i].used)
            NEAREST(pings[i].deadline);
#undef NEAREST
    /* Wake at least once a second to reap sessions and check DHCP. */
    return (int)MIN(t, 1000u);
}

int main(void)
{
    struct gz_msg m, r;
    int port = gz_port_create();

    if (port < 0)
        return 1;
    if (net_if_init(port) == 0) {
        ping_pcb = raw_new(IP_PROTO_ICMP);
        if (ping_pcb)
            raw_recv(ping_pcb, on_icmp, NULL);
    }
    if (gz_port_register(port, NET_PORT_NAME) < 0)
        return 1;

    uint32_t last_reap = gz_time_ms();
    for (;;) {
        int token = gz_recv(port, &m, net_if_state() == NET_NO_CARD ? -1 : sleep_ms());

        if (token == GZ_RECV_NOTIFY)
            net_if_poll();
        else if (token > 0) {
            reap_sessions();
            memset(&r, 0, sizeof(r));
            int rv = dispatch(&m, &r, token);
            if (rv != LATER) {
                r.w[0] = (uint32_t)rv;
                gz_reply(token, &r);
            }
        }
        uint32_t now = gz_time_ms();
        if (net_if_state() != NET_NO_CARD) {
            net_if_poll();
            sys_check_timeouts();
            expire(now);
        }
        if (now - last_reap >= 1000) {
            reap_sessions();
            last_reap = now;
        }
    }
}
