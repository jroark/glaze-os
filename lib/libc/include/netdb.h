#ifndef GLAZE_NETDB_H
#define GLAZE_NETDB_H

#include <sys/socket.h>

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE      0x1
#define AI_CANONNAME    0x2
#define AI_NUMERICHOST  0x4
#define AI_NUMERICSERV  0x400

#define EAI_AGAIN       (-3)
#define EAI_FAIL        (-4)
#define EAI_FAMILY      (-6)
#define EAI_SOCKTYPE    (-7)
#define EAI_SERVICE     (-8)
#define EAI_MEMORY      (-10)
#define EAI_SYSTEM      (-11)
#define EAI_NONAME      (-2)

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int err);

#endif
