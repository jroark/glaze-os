/* Glaze uses exact widths consistently on native hosts and 32-bit MIPS. */
#ifndef CC_INTEGERS_DEFINED
#define CC_INTEGERS_DEFINED
#include <stdint.h>
typedef int8_t cc_s8l; typedef uint8_t cc_u8l;
typedef int16_t cc_s16l; typedef uint16_t cc_u16l;
typedef int32_t cc_s32l; typedef uint32_t cc_u32l;
typedef int cc_s8f; typedef unsigned cc_u8f;
typedef int cc_s16f; typedef unsigned cc_u16f;
typedef int32_t cc_s32f; typedef uint32_t cc_u32f;
#define CC_PRIdLEAST8 "d"
#define CC_PRIiLEAST8 "i"
#define CC_PRIuLEAST8 "u"
#define CC_PRIoLEAST8 "o"
#define CC_PRIxLEAST8 "x"
#define CC_PRIXLEAST8 "X"
#define CC_PRIdLEAST16 "d"
#define CC_PRIiLEAST16 "i"
#define CC_PRIuLEAST16 "u"
#define CC_PRIoLEAST16 "o"
#define CC_PRIxLEAST16 "x"
#define CC_PRIXLEAST16 "X"
#define CC_PRIdLEAST32 "d"
#define CC_PRIiLEAST32 "i"
#define CC_PRIuLEAST32 "u"
#define CC_PRIoLEAST32 "o"
#define CC_PRIxLEAST32 "x"
#define CC_PRIXLEAST32 "X"
#define CC_PRIdFAST8 "d"
#define CC_PRIiFAST8 "i"
#define CC_PRIuFAST8 "u"
#define CC_PRIoFAST8 "o"
#define CC_PRIxFAST8 "x"
#define CC_PRIXFAST8 "X"
#define CC_PRIdFAST16 "d"
#define CC_PRIiFAST16 "i"
#define CC_PRIuFAST16 "u"
#define CC_PRIoFAST16 "o"
#define CC_PRIxFAST16 "x"
#define CC_PRIXFAST16 "X"
#define CC_PRIdFAST32 "d"
#define CC_PRIiFAST32 "i"
#define CC_PRIuFAST32 "u"
#define CC_PRIoFAST32 "o"
#define CC_PRIxFAST32 "x"
#define CC_PRIXFAST32 "X"
#endif

/* Boolean. */
#ifndef CC_BOOLEAN_DEFINED
#define CC_BOOLEAN_DEFINED

 typedef cc_u8l cc_bool;
 enum
 {
	cc_false = 0,
	cc_true = 1
 };

#endif /* CC_BOOLEAN_DEFINED */

/* Common macros. */
#ifndef CC_MIN
#define CC_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef CC_MAX
#define CC_MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef CC_CLAMP
#define CC_CLAMP(min, max, x) (CC_MAX((min), CC_MIN((max), (x))))
#endif

#ifndef CC_COUNT_OF
#define CC_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))
#endif

#ifndef CC_DIVIDE_ROUND
#define CC_DIVIDE_ROUND(a, b) (((a) + (b / 2)) / (b))
#endif

#ifndef CC_DIVIDE_CEILING
#define CC_DIVIDE_CEILING(a, b) (((a) + (b - 1)) / (b))
#endif

#ifndef CC_SIGN_EXTEND
#define CC_SIGN_EXTEND(type, bit_index, value) (((value) & (((type)1 << (bit_index)) - 1)) - ((value) & ((type)1 << (bit_index))))
#endif

#ifndef CC_SIGN_EXTEND_UINT
#define CC_SIGN_EXTEND_UINT(bit_index, value) CC_SIGN_EXTEND(unsigned int, bit_index, value)
#endif

#ifndef CC_SIGN_EXTEND_ULONG
#define CC_SIGN_EXTEND_ULONG(bit_index, value) CC_SIGN_EXTEND(unsigned long, bit_index, value)
#endif

#ifndef CC_DEGREE_TO_RADIAN
#define CC_DEGREE_TO_RADIAN(degree) ((degree) * (CC_PI / 180.0))
#endif

#ifndef CC_RADIAN_TO_DEGREE
#define CC_RADIAN_TO_DEGREE(degree) ((degree) * (180.0 / CC_PI))
#endif

#ifndef CC_STRUCT_POINTER_FROM_MEMBER_POINTER
#define CC_STRUCT_POINTER_FROM_MEMBER_POINTER(type, member, entry) ((type*)(((char*)(entry)) - offsetof(type, member)))
#endif

#ifndef CC_ATTRIBUTE_PRINTF
 /* GCC 3.2 is the earliest version of GCC of which I can find proof of supporting this. */
 #if defined(__GNUC__) && defined(__GNUC_MINOR__) && (__GNUC__ > 3 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 2))
  #define CC_ATTRIBUTE_PRINTF(a, b) __attribute__((format(printf, a, b)))
 #else
  #define CC_ATTRIBUTE_PRINTF(a, b)
 #endif
#endif

#ifndef CC_STRINGIFY
#define CC_STRINGIFY(STRING) CC_STRINGIFY2(STRING)
#define CC_STRINGIFY2(STRING) #STRING
#endif

#ifdef __cplusplus
 #ifndef CC_CPLUSPLUS
  /* MSVC is FUCKING SHIT, setting '__cplusplus' to the wrong value in a direct violation of the C++ standard. */
  #ifdef _MSVC_LANG
   #define CC_CPLUSPLUS _MSVC_LANG
  #else
   #define CC_CPLUSPLUS __cplusplus
  #endif
 #endif
#endif

/* Common constants. */
#ifndef CC_PI
#define CC_PI 3.1415926535897932384626433832795028841971693993751058209749445923078164062862089986280348253421170679
#endif

#ifndef CC_UNREACHABLE
#define CC_UNREACHABLE __builtin_unreachable()
#endif
