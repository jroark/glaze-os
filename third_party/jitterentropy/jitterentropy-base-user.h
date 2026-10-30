/* Glaze port of Jitterentropy 3.6.3. Core source and health limits unchanged. */
#ifndef _JITTERENTROPY_BASE_USER_H
#define _JITTERENTROPY_BASE_USER_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#ifdef GLAZE_ENTROPY_TEST
uint64_t entropy_test_time(void);
static inline void jent_get_nstime(uint64_t *out) { *out = entropy_test_time(); }
static inline void jent_yield(void) { }
#else
#include <glaze/sys.h>
static inline void jent_get_nstime(uint64_t *out) { *out = gz_time_us(); }
static inline void jent_yield(void) { gz_yield(); }
#endif
/* The upstream internal header defines the same helper. */
#undef ARRAY_SIZE
static inline void *jent_zalloc(size_t size) { return calloc(1, size); }
static inline void jent_memset_secure(void *p, size_t size)
{
    volatile uint8_t *bytes = p;
    while (size--) *bytes++ = 0;
}
static inline void jent_zfree(void *p, unsigned size)
{
    if (p) jent_memset_secure(p, size);
    free(p);
}
/* Enables upstream continuous health tests; this is not a certification. */
static inline int jent_fips_enabled(void) { return 1; }
static inline long jent_ncpu(void) { return 1; }
static inline uint32_t jent_cache_size_roundup(void) { return 32768; }
static inline uint64_t rol64(uint64_t x, int n)
{
    return (x << (n & 63)) | (x >> ((64 - n) & 63));
}
#endif
