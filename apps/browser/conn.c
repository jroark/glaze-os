/* Non-blocking TCP connections over BSD sockets. */

#include "net.h"
#include "web.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int we_of(int e)
{
    switch (e) {
    case ENETDOWN: return WE_NETDOWN;
    case ECONNREFUSED: return WE_CONNECT;
    case EHOSTUNREACH: case ENETUNREACH: return WE_CONNECT;
    case ETIMEDOUT: return WE_TIMEOUT;
    case ECONNRESET: case EPIPE: return WE_RESET;
    case ENOMEM: case ENOBUFS: return WE_NOMEM;
    default: return WE_CONNECT;
    }
}

int conn_open(struct conn *c, uint32_t addr, int port)
{
    struct sockaddr_in sa;

    c->connected = false;
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0)
        return we_of(errno);
    fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL, 0) | O_NONBLOCK);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = addr;
    if (connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        c->connected = true;
        return 0;
    }
    if (errno == EINPROGRESS)
        return 0;
    int e = we_of(errno);
    conn_close(c);
    return e;
}

int conn_ready(struct conn *c)
{
    struct pollfd p = { c->fd, POLLOUT, 0 };

    if (c->connected)
        return 1;
    if (poll(&p, 1, 0) <= 0)
        return 0;
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err)
        return we_of(err);
    c->connected = true;
    return 1;
}

int conn_read(struct conn *c, void *buf, int n)
{
    ssize_t r = recv(c->fd, buf, (size_t)n, 0);

    if (r >= 0)
        return (int)r;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? -CONN_AGAIN : we_of(errno);
}

int conn_write(struct conn *c, const void *buf, int n)
{
    ssize_t r = send(c->fd, buf, (size_t)n, 0);

    if (r >= 0)
        return (int)r;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? -CONN_AGAIN : we_of(errno);
}

void conn_close(struct conn *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->connected = false;
}
