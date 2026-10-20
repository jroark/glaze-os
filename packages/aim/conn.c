/*
 * The connection to an OSCAR server (glaze/net.h). Resolving and connecting
 * can take seconds, so a thread does them while the UI keeps drawing; the
 * main thread then sends, and reads without waiting, on its own.
 *
 * Each attempt has a generation number. An attempt that finishes after it
 * was abandoned (conn_close, or a newer conn_start) closes its socket
 * instead of publishing it.
 */

#include <glaze/net.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <stdlib.h>
#include <string.h>
#include "aim.h"
#include "oscar.h"

#define WAIT_UP_MS      8000
#define RESOLVE_MS      10000
#define CONNECT_MS      15000

static struct gz_mutex lock = GZ_MUTEX_INIT;
static int generation;
static int state = CONN_NONE;
static int sock = -1;
static char error[96];

struct attempt {
    int generation;
    char host[OSCAR_HOST_MAX];
    uint16_t port;
};

static void finish(const struct attempt *a, int s, const char *why)
{
    gz_mutex_lock(&lock);
    if (a->generation != generation) {
        gz_mutex_unlock(&lock);
        if (s >= 0)
            net_close(s);
        return;
    }
    if (s >= 0) {
        sock = s;
        state = CONN_UP;
    } else {
        strlcpy(error, why, sizeof(error));
        state = CONN_FAILED;
    }
    gz_mutex_unlock(&lock);
}

static void connect_thread(void *arg)
{
    struct attempt *a = arg;
    struct net_status st;
    uint32_t addr;
    int s, rv;

    if (net_status(&st) < 0 || st.state == NET_NO_CARD) {
        finish(a, -1, "There is no network. Insert an Ethernet card.");
    } else if (net_wait_up(WAIT_UP_MS) < 0) {
        finish(a, -1, "The network isn't ready yet.");
    } else if (net_aton(a->host, &addr) < 0 && net_resolve(a->host, &addr, RESOLVE_MS) < 0) {
        finish(a, -1, "The server's name could not be found.");
    } else if ((s = net_socket(NET_TCP)) < 0) {
        finish(a, -1, net_strerror(s));
    } else if ((rv = net_connect(s, addr, a->port, CONNECT_MS)) < 0) {
        net_close(s);
        finish(a, -1, rv == GZ_ETIMEDOUT ? "The server didn't answer." : "The server refused the connection.");
    } else {
        net_sockopt(s, NET_OPT_NODELAY, 1);
        finish(a, s, NULL);
    }
    free(a);
}

void conn_start(const char *host, uint16_t port)
{
    struct attempt *a = malloc(sizeof(*a));

    conn_close();
    gz_mutex_lock(&lock);
    state = CONN_CONNECTING;
    if (!a) {
        strlcpy(error, "Out of memory.", sizeof(error));
        state = CONN_FAILED;
        gz_mutex_unlock(&lock);
        return;
    }
    a->generation = generation;
    strlcpy(a->host, host, sizeof(a->host));
    a->port = port;
    gz_mutex_unlock(&lock);
    if (gz_thread_create(connect_thread, a) < 0) {
        free(a);
        gz_mutex_lock(&lock);
        strlcpy(error, "Couldn't start connecting.", sizeof(error));
        state = CONN_FAILED;
        gz_mutex_unlock(&lock);
    }
}

int conn_state(void)
{
    gz_mutex_lock(&lock);
    int s = state;
    gz_mutex_unlock(&lock);
    return s;
}

const char *conn_error(void)
{
    return error;
}

int conn_send(const void *buf, size_t len)
{
    if (sock < 0)
        return -1;
    int n = net_send(sock, buf, (uint32_t)len);
    return n == (int)len ? 0 : -1;
}

int conn_recv(void *buf, size_t len)
{
    if (sock < 0)
        return -1;
    int n = net_recv(sock, buf, (uint32_t)len, 0);
    if (n == GZ_ETIMEDOUT)
        return 0;
    return n > 0 ? n : -1;
}

void conn_close(void)
{
    gz_mutex_lock(&lock);
    generation++;
    if (sock >= 0)
        net_close(sock);
    sock = -1;
    state = CONN_NONE;
    gz_mutex_unlock(&lock);
}
