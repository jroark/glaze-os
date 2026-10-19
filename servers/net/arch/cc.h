/* lwIP port: compiler and platform definitions for Glaze. */
#ifndef GLAZE_LWIP_CC_H
#define GLAZE_LWIP_CC_H

#include <glaze/types.h>

#define BYTE_ORDER          LITTLE_ENDIAN
#define LWIP_NO_CTYPE_H     1
#define LWIP_NO_LIMITS_H    1   /* the cross compiler's limits.h pulls in glibc */
#define INT_MAX             0x7FFFFFFF

/* libc's inttypes.h has no 8-bit or size_t formats. */
#define X8_F    "02x"
#define SZT_F   "u"

void net_lwip_assert(const char *msg, const char *file, int line) __attribute__((noreturn));
uint32_t net_lwip_rand(void);

#define LWIP_PLATFORM_DIAG(x)       do { } while (0)
#define LWIP_PLATFORM_ASSERT(x)     net_lwip_assert((x), __FILE__, __LINE__)
#define LWIP_RAND()                 net_lwip_rand()

#endif
