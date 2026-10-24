/* Glaze portability definitions for the selected Mednafen C core. */
#ifndef GLAZE_MDFN_TYPES_H
#define GLAZE_MDFN_TYPES_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
typedef uint8_t uint8; typedef int8_t int8;
typedef uint16_t uint16; typedef int16_t int16;
typedef uint32_t uint32; typedef int32_t int32;
typedef uint64_t uint64; typedef int64_t int64;
#define INLINE inline
#define MDFN_FASTCALL
#define MDFN_COLD
#define MDFN_HOT
#define NO_INLINE __attribute__((noinline))
#define MDFN_ALIGN(n) __attribute__((aligned(n)))
#define MDFN_LIKELY(n) __builtin_expect(!!(n),1)
#define MDFN_UNLIKELY(n) __builtin_expect(!!(n),0)
typedef struct StateMem StateMem;
#endif
