/*
 * Networking: client API and the protocol of the network server
 * (servers/net), which runs lwIP over the kernel's NE2000 driver.
 *
 * Sessions work like the file server's: a client sends NET_HELLO with its
 * pid and gets a session index plus a shared-memory buffer, which carries
 * host names and data. Calls that wait (connect, accept, send, recv,
 * resolve, ping, poll) run on the server's worker threads and are
 * answered when they complete or time out, so a client blocks in its send
 * while the server keeps serving others. A timeout of 0 doesn't wait:
 * recv and accept fail with GZ_ETIMEDOUT and connect returns
 * GZ_EINPROGRESS (then poll for NET_POLLOUT). Sockets of exited processes
 * are closed. libc's BSD sockets (sys/socket.h) are built on this API.
 *
 * Addresses are IPv4 in network byte order (as lwIP's ip4_addr_t holds
 * them); ports are host order. Timeouts are in milliseconds, and
 * NET_FOREVER waits indefinitely.
 */
#ifndef GLAZE_NET_H
#define GLAZE_NET_H

#include <glaze/abi.h>

#define NET_PORT_NAME   "net"
#define NET_BUF_SIZE    8192u
#define NET_IO_MAX      4096u       /* data bytes per request */
#define NET_HOST_MAX    128
#define NET_FOREVER     (-1)

#define NET_IP(a, b, c, d) \
    ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

/* Requests: w0 = op, w1 = session; replies: w0 = result (>= 0 or error). */
enum {
    NET_HELLO = 1,      /* w1 = pid -> w1 = session, w2 = shm id */
    NET_STATUS,         /* -> buf: struct net_status; w1 = -1: -> reply w1... instead */
    NET_RESOLVE,        /* buf: host name, w2 = timeout -> w1 = address */
    NET_SOCKET,         /* w2 = NET_TCP or NET_UDP -> socket */
    NET_CONNECT,        /* w2 = socket, w3 = address, w4 = port, w5 = timeout (0: start) */
    NET_BIND,           /* w2 = socket, w3 = port (0: any) -> port */
    NET_LISTEN,         /* w2 = socket (TCP, bound) */
    NET_ACCEPT,         /* w2 = socket, w3 = timeout -> socket, w1 = address, w2 = port */
    NET_SEND,           /* w2 = socket, w3 = length, w4 = NET_MSG_*, buf: data -> bytes queued */
    NET_RECV,           /* w2 = socket, w3 = max, w4 = timeout, w5 = NET_MSG_* -> bytes
                           (0: closed), buf: data */
    NET_SENDTO,         /* w2 = socket, w3 = length, w4 = address, w5 = port, buf: data */
    NET_RECVFROM,       /* w2 = socket, w3 = max, w4 = timeout -> bytes, w1 = address,
                           w2 = port, buf: data */
    NET_CLOSE,          /* w2 = socket */
    NET_PING,           /* w2 = address, w3 = timeout -> round trip in ms */
    NET_POLL,           /* w2 = count, w3 = timeout, buf: struct net_pollfd[] -> ready count */
    NET_SOCKOPT,        /* w2 = socket, w3 = NET_OPT_*, w4 = value -> value */
    NET_SHUTDOWN,       /* w2 = socket, w3 = NET_SHUT_* */
    NET_NAME,           /* w2 = socket, w3 = 0 local / 1 peer -> w1 = address, w2 = port */
    NET_WIFI_STATUS, NET_WIFI_SCAN, NET_WIFI_CONNECT, NET_WIFI_DISCONNECT,
};

#define NET_MSG_DONTWAIT    0x1     /* send: queue what fits, GZ_ETIMEDOUT if nothing */
#define NET_MSG_PEEK        0x2     /* recv: leave the data queued */

/* Poll events (the POSIX values). */
#define NET_POLLIN          0x01
#define NET_POLLOUT         0x04
#define NET_POLLERR         0x08
#define NET_POLLHUP         0x10
#define NET_POLLNVAL        0x20
#define NET_POLL_MAX        32

struct net_pollfd {
    int32_t sock;
    int16_t events, revents;
};

enum {
    NET_OPT_ERROR = 1,  /* -> pending error (GZ_*) or 0, and clears it */
    NET_OPT_NODELAY,    /* TCP_NODELAY */
    NET_OPT_KEEPALIVE,
    NET_OPT_REUSEADDR,
};

enum { NET_SHUT_RD, NET_SHUT_WR, NET_SHUT_RDWR };

enum { NET_TCP = 1, NET_UDP = 2 };

enum {
    NET_NO_CARD = 0,    /* no supported network card */
    NET_CONFIGURING,    /* waiting for DHCP */
    NET_UP,             /* address from DHCP */
    NET_UP_STATIC,      /* no DHCP answer; using the fallback address */
    NET_DISCONNECTED,   /* wireless card, no association */
    NET_ASSOCIATING,    /* wireless association pending */
};

struct net_status {
    uint32_t state;
    uint8_t mac[6];
    uint16_t reserved;
    uint32_t addr, mask, gateway, dns;
    uint32_t rx_frames, tx_frames, rx_dropped;
};

/* ---- client API (libc) --------------------------------------------------- */

int net_status(struct net_status *st);
/* Waits up to timeout_ms for an address; returns 0 or GZ_ENETDOWN. */
int net_wait_up(int timeout_ms);
int net_resolve(const char *host, uint32_t *addr, int timeout_ms);
int net_socket(int type);
int net_connect(int s, uint32_t addr, uint16_t port, int timeout_ms);
int net_bind(int s, uint16_t port);
int net_listen(int s);
int net_accept(int s, uint32_t *addr, uint16_t *port, int timeout_ms);
/* Sends everything, waiting for buffer space; bytes sent or an error. */
int net_send(int s, const void *buf, uint32_t len);
/* Bytes received (at most len), 0 once the peer has closed, or an error. */
int net_recv(int s, void *buf, uint32_t len, int timeout_ms);
int net_sendto(int s, const void *buf, uint32_t len, uint32_t addr, uint16_t port);
int net_recvfrom(int s, void *buf, uint32_t len, uint32_t *addr, uint16_t *port, int timeout_ms);
int net_close(int s);
int net_ping(uint32_t addr, int timeout_ms);
int net_send_flags(int s, const void *buf, uint32_t len, int flags);
int net_recv_flags(int s, void *buf, uint32_t len, int timeout_ms, int flags);
int net_poll(struct net_pollfd *fds, int n, int timeout_ms);
int net_sockopt(int s, int opt, int value);
int net_shutdown(int s, int how);
int net_name(int s, bool peer, uint32_t *addr, uint16_t *port);

/* "a.b.c.d" into out (at least 16 bytes); returns out. */
char *net_ntoa(uint32_t addr, char *out);
/* Parses "a.b.c.d"; returns 0 or GZ_EINVAL. */
int net_aton(const char *s, uint32_t *addr);
const char *net_strerror(int err);

#endif
