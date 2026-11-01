#pragma once
#ifndef _WIN32
#include_next <sys/random.h>
#else
#include <windows.h>
#include <bcrypt.h>
static inline int getentropy(void *p, size_t n) {
    return BCryptGenRandom(NULL,p,(ULONG)n,BCRYPT_USE_SYSTEM_PREFERRED_RNG) ? -1 : 0;
}
#endif
