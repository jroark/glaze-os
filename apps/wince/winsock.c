/* CE Winsock 1.x layouts translated to Glaze's IPv4 socket services. */
#include "runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <glaze/net.h>
#include <glaze/thread.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
struct socket_thread {
    char name[NET_HOST_MAX], dotted[16];
    CW host[4], addresses[2], aliases[1], address;
};
static struct socket_thread *socket_thread(void)
{
    if (!gz_self()->tls[0])
        gz_self()->tls[0] = calloc(1, sizeof(struct socket_thread));
    return gz_self()->tls[0];
}
static CW socket_error(int error)
{
    switch (error) {
    case EBADF:
    case ENOTSOCK:
        return 10038;
    case EAGAIN:
        return 10035;
    case EINPROGRESS:
        return 10036;
    case EALREADY:
        return 10037;
    case ECONNREFUSED:
        return 10061;
    case ECONNRESET:
        return 10054;
    case ECONNABORTED:
        return 10053;
    case ETIMEDOUT:
        return 10060;
    case ENETDOWN:
        return 10050;
    case ENETUNREACH:
        return 10051;
    case EHOSTUNREACH:
        return 10065;
    case EISCONN:
        return 10056;
    case ENOTCONN:
        return 10057;
    case EADDRINUSE:
        return 10048;
    case ENOMEM:
    case ENOBUFS:
        return 10055;
    case EAFNOSUPPORT:
        return 10047;
    case ENOPROTOOPT:
        return 10042;
    default:
        return 10022;
    }
}
static CW result(int r)
{
    if (r < 0)
        ce_error = socket_error(errno);
    return (CW)r;
}
CE_API(socket) { return result(socket(a0, a1, a2)); }
CE_API(closesocket) { return result(close(a0)); }
CE_API(connect) { return result(connect(a0, PTR(struct sockaddr, a1), a2)); }
CE_API(bind) { return result(bind(a0, PTR(struct sockaddr, a1), a2)); }
CE_API(listen) { return result(listen(a0, a1)); }
CE_API(accept) { return result(accept(a0, PTR(struct sockaddr, a1), PTR(socklen_t, a2))); }
CE_API(getpeername) { return result(getpeername(a0, PTR(struct sockaddr, a1), PTR(socklen_t, a2))); }
CE_API(getsockname) { return result(getsockname(a0, PTR(struct sockaddr, a1), PTR(socklen_t, a2))); }
CE_API(sock_shutdown) { return result(shutdown(a0, a1)); }
CE_API(sock_send) { return result(send(a0, PTR(void, a1), a2, a3)); }
CE_API(recv) { return result(recv(a0, PTR(void, a1), a2, a3)); }
CE_API(sendto) { return result(sendto(a0, PTR(void, a1), a2, a3, PTR(struct sockaddr, a4), a5)); }
CE_API(recvfrom)
{
    return result(recvfrom(a0, PTR(void, a1), a2, a3, PTR(struct sockaddr, a4), PTR(socklen_t, a5)));
}
CE_API(htons) { return htons(a0); }
CE_API(ntohs) { return ntohs(a0); }
CE_API(htonl) { return htonl(a0); }
CE_API(ntohl) { return ntohl(a0); }
CE_API(ioctlsocket)
{
    if (a1 != 0x8004667eu || !a2) {
        ce_error = 10045;
        return (CW)-1;
    }
    int flags = fcntl(a0, F_GETFL, 0);
    if (flags < 0)
        return result(-1);
    return result(fcntl(a0, F_SETFL, *PTR(CW, a2) ? flags | O_NONBLOCK : flags & ~O_NONBLOCK));
}
static int option(CW level, CW name)
{
    if (level != 0xffffu)
        return level == IPPROTO_TCP && name == 1 ? TCP_NODELAY : -1;
    switch (name) {
    case 4:
        return SO_REUSEADDR;
    case 8:
        return SO_KEEPALIVE;
    case 0x1007:
        return SO_ERROR;
    case 0x1008:
        return SO_TYPE;
    case 0x1005:
        return SO_SNDTIMEO;
    case 0x1006:
        return SO_RCVTIMEO;
    default:
        return -1;
    }
}
CE_API(getsockopt)
{
    int opt = option(a1, a2);
    if (opt < 0) {
        ce_error = 10042;
        return (CW)-1;
    }
    if (a1 == 0xffffu && (a2 == 0x1005 || a2 == 0x1006)) {
        if (!a3 || !a4 || *PTR(CW, a4) < 4) {
            ce_error = 10014;
            return (CW)-1;
        }
        struct timeval tv = {0};
        socklen_t len = sizeof(tv);
        int r = getsockopt(a0, SOL_SOCKET, opt, &tv, &len);
        if (r < 0)
            return result(r);
        uint64_t ms = (uint64_t)tv.tv_sec * 1000 + (tv.tv_usec + 999) / 1000;
        *PTR(CW, a3) = ms > UINT32_MAX ? UINT32_MAX : (CW)ms;
        *PTR(CW, a4) = 4;
        return 0;
    }
    int r = getsockopt(a0, a1 == 0xffffu ? SOL_SOCKET : a1, opt, PTR(void, a3), PTR(socklen_t, a4));
    if (!r && a1 == 0xffffu && a2 == 0x1007 && a3 && *PTR(CW, a3))
        *PTR(CW, a3) = socket_error(*PTR(CW, a3));
    return result(r);
}
CE_API(setsockopt)
{
    int opt = option(a1, a2);
    if (opt < 0) {
        ce_error = 10042;
        return (CW)-1;
    }
    if (a1 == 0xffffu && (a2 == 0x1005 || a2 == 0x1006)) {
        if (!a3 || a4 < 4) {
            ce_error = 10014;
            return (CW)-1;
        }
        CW ms = *PTR(CW, a3);
        struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
        return result(setsockopt(a0, SOL_SOCKET, opt, &tv, sizeof(tv)));
    }
    return result(setsockopt(a0, a1 == 0xffffu ? SOL_SOCKET : a1, opt, PTR(void, a3), a4));
}
CE_API(gethostbyname)
{
    struct socket_thread *t = socket_thread();
    if (!a0 || !t || strlen(PTR(char, a0)) >= sizeof(t->name)) {
        ce_error = 10022;
        return 0;
    }
    strlcpy(t->name, PTR(char, a0), sizeof(t->name));
    if (net_resolve(t->name, &t->address, 10000) < 0) {
        ce_error = 11001;
        return 0;
    }
    t->aliases[0] = 0;
    t->addresses[0] = WORD(&t->address);
    t->addresses[1] = 0;
    t->host[0] = WORD(t->name);
    t->host[1] = WORD(t->aliases);
    t->host[2] = 2u | (4u << 16);
    t->host[3] = WORD(t->addresses);
    return WORD(t->host);
}
CE_API(inet_ntoa)
{
    struct socket_thread *t = socket_thread();
    if (!t) {
        ce_error = 10055;
        return 0;
    }
    snprintf(t->dotted, sizeof(t->dotted), "%u.%u.%u.%u", a0 & 255, (a0 >> 8) & 255, (a0 >> 16) & 255,
             a0 >> 24);
    return WORD(t->dotted);
}
CE_API(__WSAFDIsSet)
{
    if (!a1)
        return 0;
    CW *set = PTR(CW, a1);
    if (set[0] > 64)
        return 0;
    for (unsigned i = 0; i < set[0]; i++)
        if (set[i + 1] == a0)
            return 1;
    return 0;
}
CE_API(select)
{
    struct pollfd fds[32];
    unsigned count = 0;
    CW *sets[] = {PTR(CW, a1), PTR(CW, a2), PTR(CW, a3)};
    const int masks[] = {POLLIN, POLLOUT, POLLPRI};
    for (unsigned side = 0; side < 3; side++)
        if (sets[side]) {
            if (sets[side][0] > 64) {
                ce_error = 10022;
                return (CW)-1;
            }
            for (unsigned n = 0; n < sets[side][0]; n++) {
                int fd = sets[side][n + 1];
                unsigned i = 0;
                while (i < count && fds[i].fd != fd)
                    i++;
                if (i == count) {
                    if (count == ARRAY_SIZE(fds)) {
                        ce_error = 10022;
                        return (CW)-1;
                    }
                    fds[count++] = (struct pollfd){fd, 0, 0};
                }
                fds[i].events |= masks[side];
            }
        }
    int timeout = -1;
    if (a4) {
        const int32_t *tv = PTR(int32_t, a4);
        if (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000) {
            ce_error = 10022;
            return (CW)-1;
        }
        uint64_t ms = (uint64_t)tv[0] * 1000 + (tv[1] + 999) / 1000;
        timeout = ms > INT32_MAX ? INT32_MAX : (int)ms;
    }
    int r = poll(fds, count, timeout);
    if (r < 0)
        return result(r);
    unsigned total = 0;
    for (unsigned side = 0; side < 3; side++)
        if (sets[side]) {
            unsigned nready = 0;
            for (unsigned n = 0; n < sets[side][0]; n++)
                for (unsigned i = 0; i < count; i++)
                    if (fds[i].fd == (int)sets[side][n + 1]) {
                        if (fds[i].revents & POLLNVAL) {
                            ce_error = 10038;
                            return (CW)-1;
                        }
                        if (fds[i].revents & (masks[side] | (side == 0   ? POLLHUP | POLLERR
                                                             : side == 2 ? POLLERR
                                                                         : 0)))
                            sets[side][++nready] = fds[i].fd;
                        break;
                    }
            sets[side][0] = nready;
            total += nready;
        }
    return total;
}
CE_API(WSAGetLastError) { return ce_error; }
CE_API(WSASetLastError)
{
    ce_error = a0;
    return 0;
}
CE_API(WSAStartup)
{
    if (!a1)
        return 10014;
    if ((a0 & 255) != 1 && (a0 & 255) != 2)
        return 10092;
    memset(PTR(void, a1), 0, 400);
    ce_put16(PTR(void, a1), a0 & 0xffff);
    ce_put16(PTR(uint8_t, a1) + 2, 0x202);
    return 0;
}
CE_API(WSACleanup) { return 0; }
static const struct ce_export exports[] = {CE_EXPORT(socket),
                                           CE_EXPORT(closesocket),
                                           CE_EXPORT(connect),
                                           CE_EXPORT(bind),
                                           CE_EXPORT(listen),
                                           CE_EXPORT(accept),
                                           CE_EXPORT(getpeername),
                                           CE_EXPORT(getsockname),
                                           {"shutdown", (void *)ce_sock_shutdown},
                                           {"send", (void *)ce_sock_send},
                                           CE_EXPORT(recv),
                                           CE_EXPORT(sendto),
                                           CE_EXPORT(recvfrom),
                                           CE_EXPORT(htons),
                                           CE_EXPORT(ntohs),
                                           CE_EXPORT(htonl),
                                           CE_EXPORT(ntohl),
                                           CE_EXPORT(ioctlsocket),
                                           CE_EXPORT(getsockopt),
                                           CE_EXPORT(setsockopt),
                                           CE_EXPORT(gethostbyname),
                                           CE_EXPORT(inet_ntoa),
                                           CE_EXPORT(__WSAFDIsSet),
                                           CE_EXPORT(select),
                                           CE_EXPORT(WSAGetLastError),
                                           CE_EXPORT(WSASetLastError),
                                           CE_EXPORT(WSAStartup),
                                           CE_EXPORT(WSACleanup)};
void *ce_resolve_winsock(const char *dll, const char *name)
{
    if (strcasecmp(dll, "winsock.dll") && strcasecmp(dll, "ws2.dll"))
        return NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
