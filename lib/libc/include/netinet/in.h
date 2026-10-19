#ifndef GLAZE_NETINET_IN_H
#define GLAZE_NETINET_IN_H

#include <sys/socket.h>

#define IPPROTO_IP      0
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_IPV6    41

#define INADDR_ANY      ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)
#define INADDR_NONE     ((in_addr_t)0xffffffff)

#define INET_ADDRSTRLEN     16
#define INET6_ADDRSTRLEN    46

struct in_addr {
    in_addr_t s_addr;       /* network byte order */
};

struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;     /* network byte order */
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

/* The VR4131 runs little-endian. */
static inline uint16_t htons(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v)
{
    return v << 24 | (v & 0xFF00) << 8 | (v >> 8 & 0xFF00) | v >> 24;
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

#define TCP_NODELAY     1

#endif
