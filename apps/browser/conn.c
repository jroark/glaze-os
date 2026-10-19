/*
 * Non-blocking TCP connections over BSD sockets, optionally under TLS
 * (wolfSSL, set up in tls.c). wolfSSL reads and writes the socket through
 * the callbacks here, which turn EAGAIN into its "want read/write", so the
 * handshake advances a little on each conn_ready().
 */

#include "net.h"
#include "web.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>

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

static bool again(int e)
{
    return e == EAGAIN || e == EWOULDBLOCK || e == EINTR;
}

/* ---- wolfSSL I/O callbacks ---------------------------------------------------- */

static int tls_recv(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    struct conn *c = ctx;
    ssize_t r = recv(c->fd, buf, (size_t)sz, 0);

    (void)ssl;
    if (r > 0)
        return (int)r;
    if (r == 0)
        return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    if (again(errno))
        return WOLFSSL_CBIO_ERR_WANT_READ;
    c->err = errno;
    return errno == ECONNRESET ? WOLFSSL_CBIO_ERR_CONN_RST : WOLFSSL_CBIO_ERR_GENERAL;
}

static int tls_send(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    struct conn *c = ctx;
    ssize_t r = send(c->fd, buf, (size_t)sz, 0);

    (void)ssl;
    if (r >= 0)
        return (int)r;
    if (again(errno))
        return WOLFSSL_CBIO_ERR_WANT_WRITE;
    c->err = errno;
    return errno == ECONNRESET || errno == EPIPE ? WOLFSSL_CBIO_ERR_CONN_RST : WOLFSSL_CBIO_ERR_GENERAL;
}

/* WE_* for a failed wolfSSL call: the socket's error if it had one. */
static int tls_fail(struct conn *c, int ret)
{
    int e = wolfSSL_get_error(c->tls, ret);

    if (c->err)
        return we_of(c->err);
    return tls_error(e);
}

/* ---- connections -------------------------------------------------------------- */

int conn_open(struct conn *c, uint32_t addr, int port, const char *tls_host)
{
    struct sockaddr_in sa;

    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->port = port;
    if (tls_host) {
        int err;
        c->host = strdup(tls_host);
        if (!c->host)
            return WE_NOMEM;
        c->tls = tls_new(tls_host, port, &err);
        if (!c->tls) {
            free(c->host);
            c->host = NULL;
            return err;
        }
        wolfSSL_SSLSetIORecv(c->tls, tls_recv);
        wolfSSL_SSLSetIOSend(c->tls, tls_send);
        wolfSSL_SetIOReadCtx(c->tls, c);
        wolfSSL_SetIOWriteCtx(c->tls, c);
    }
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) {
        int e = we_of(errno);
        conn_close(c);
        return e;
    }
    fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL, 0) | O_NONBLOCK);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = addr;
    if (connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        c->tcp_up = true;
        c->connected = !c->tls;
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
    if (c->connected)
        return 1;
    if (!c->tcp_up) {
        struct pollfd p = { c->fd, POLLOUT, 0 };
        if (poll(&p, 1, 0) <= 0)
            return 0;
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err)
            return we_of(err);
        c->tcp_up = true;
        if (!c->tls) {
            c->connected = true;
            return 1;
        }
    }
    /* The TLS handshake, as far as the socket allows. */
    int ret = wolfSSL_connect(c->tls);
    if (ret == WOLFSSL_SUCCESS) {
        c->connected = true;
        return 1;
    }
    int e = wolfSSL_get_error(c->tls, ret);
    if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE)
        return 0;
    return tls_fail(c, ret);
}

int conn_read(struct conn *c, void *buf, int n)
{
    if (c->tls) {
        int r = wolfSSL_read(c->tls, buf, n);
        if (r > 0)
            return r;
        int e = wolfSSL_get_error(c->tls, r);
        if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE)
            return -CONN_AGAIN;
        /* close_notify, or the server just closing the socket: EOF (the
         * HTTP layer checks the length). */
        if (e == WOLFSSL_ERROR_ZERO_RETURN || e == SOCKET_PEER_CLOSED_E || (!c->err && r == 0))
            return 0;
        return tls_fail(c, r);
    }
    ssize_t r = recv(c->fd, buf, (size_t)n, 0);

    if (r >= 0)
        return (int)r;
    return again(errno) ? -CONN_AGAIN : we_of(errno);
}

int conn_write(struct conn *c, const void *buf, int n)
{
    if (c->tls) {
        int r = wolfSSL_write(c->tls, buf, n);
        if (r > 0)
            return r;
        int e = wolfSSL_get_error(c->tls, r);
        if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE)
            return -CONN_AGAIN;
        return tls_fail(c, r);
    }
    ssize_t r = send(c->fd, buf, (size_t)n, 0);

    if (r >= 0)
        return (int)r;
    return again(errno) ? -CONN_AGAIN : we_of(errno);
}

void conn_close(struct conn *c)
{
    if (c->tls) {
        if (c->connected && !c->err)
            wolfSSL_shutdown(c->tls);   /* best effort: one non-blocking try */
        tls_free(c->tls, c->connected ? c->host : NULL, c->port);
        c->tls = NULL;
    }
    free(c->host);
    c->host = NULL;
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->connected = c->tcp_up = false;
}
