/*
 * BSD sockets (lib/libc/socket.c), served by lwIP's sockets in the network
 * server through glaze/net.h. IPv4 TCP and UDP only. Sockets are the only
 * descriptors, so an fd is a network socket handle. The declarations
 * follow POSIX so programs written against them also build on a host.
 */
#ifndef GLAZE_SYS_SOCKET_H
#define GLAZE_SYS_SOCKET_H

#include <sys/time.h>
#include <sys/types.h>

#define AF_UNSPEC       0
#define AF_INET         2
#define AF_INET6        10
#define PF_UNSPEC       AF_UNSPEC
#define PF_INET         AF_INET
#define PF_INET6        AF_INET6

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_NONBLOCK   0x800

#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_ERROR        4
#define SO_TYPE         3
#define SO_KEEPALIVE    9
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21

#define MSG_PEEK        0x2
#define MSG_DONTWAIT    0x40
#define MSG_NOSIGNAL    0x4000

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char __ss_pad[126];
};

int socket(int domain, int type, int protocol);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
int shutdown(int fd, int how);
int setsockopt(int fd, int level, int name, const void *val, socklen_t len);
int getsockopt(int fd, int level, int name, void *val, socklen_t *len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);

#endif
