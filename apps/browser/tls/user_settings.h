/*
 * wolfSSL configuration for the browser (built with -DWOLFSSL_USER_SETTINGS).
 *
 * A TLS 1.2/1.3 client with certificate verification and nothing else: no
 * server, no file system, no threads, no OpenSSL compatibility layer. Math
 * is sp_int with 32-bit digits, since the VR4131 runs 32-bit code only
 * (products use multu; 64-bit division comes from lib/libc/divdi3.c).
 * The same file configures the native build in host/ (__STDC_HOSTED__).
 */
#ifndef GLAZE_WOLFSSL_USER_SETTINGS_H
#define GLAZE_WOLFSSL_USER_SETTINGS_H

/* ---- platform ---------------------------------------------------------- */

#define SINGLE_THREADED
#define NO_FILESYSTEM
#define NO_WRITEV
#define NO_ERROR_STRINGS
#define NO_MAIN_DRIVER
#define WOLFSSL_USER_IO         /* conn.c supplies the send/receive callbacks */
#define WOLFSSL_NO_SOCK
#define WOLFSSL_IGNORE_FILE_WARN
#define WOLFSSL_SMALL_STACK
#define WOLFSSL_NO_ASM
#define NO_DEV_RANDOM
#define NO_SESSION_CACHE_REF

#if !__STDC_HOSTED__
/* Glaze isn't Unix: keep wolfSSL away from open(), /dev/urandom and such. */
#undef __unix__
#undef __linux__
#include <strings.h>            /* strcasecmp, for XSTRCASECMP */
#define SIZEOF_LONG             4
#define SIZEOF_LONG_LONG        8
#define WOLFSSL_GENERAL_ALIGNMENT 4
#define WOLFSSL_USE_ALIGN       /* lw/sw fault on unaligned addresses */
#define NO_STDIO_FILESYSTEM
#define WOLFSSL_NO_FENCE
#define NO_INLINE_ASM_CHECKS
#endif

/* Time: seconds since 1970 from the RTC (local time; see the skew below).
 * On the device, wolfSSL's own gmtime_r, time_t and struct tm. */
#define TIME_OVERRIDES
#if __STDC_HOSTED__
#define HAVE_TIME_T_TYPE
#define HAVE_TM_TYPE
#include <time.h>
#else
#define WOLFSSL_GMTIME
#endif
#define XTIME                   glaze_tls_time
#define XGMTIME                 gmtime_r
/* The RTC has no time zone, so allow a day either way. */
#define WOLFSSL_BEFORE_DATE_CLOCK_SKEW  86400
#define WOLFSSL_AFTER_DATE_CLOCK_SKEW   86400

/* Seeds for the Hash DRBG (tls.c). */
#define CUSTOM_RAND_GENERATE_SEED glaze_tls_seed
#define CUSTOM_RAND_TYPE        unsigned char
int glaze_tls_seed(unsigned char *out, unsigned int n);

/* ---- protocol ---------------------------------------------------------- */

#define NO_WOLFSSL_SERVER
#define WOLFSSL_TLS13
#define NO_OLD_TLS              /* TLS 1.2 and 1.3 only */
#define HAVE_TLS_EXTENSIONS
#define HAVE_SUPPORTED_CURVES
#define HAVE_EXTENDED_MASTER
#define HAVE_ENCRYPT_THEN_MAC
#define HAVE_SNI
#define WOLFSSL_IP_ALT_NAME     /* https://192.168.1.1/ against IP names */
#define HAVE_SESSION_TICKET
#define HAVE_HKDF
#define SMALL_SESSION_CACHE
#define NO_PSK
#define WOLFSSL_NO_TLS12_RENEGOTIATION
#define HAVE_SERVER_RENEGOTIATION_INFO

/* ---- algorithms -------------------------------------------------------- */

#define WOLFSSL_SP_MATH_ALL
#define WOLFSSL_SP_SMALL
#define SP_WORD_SIZE            32
#define SP_INT_BITS             4096    /* RSA-4096 roots */
#define TFM_TIMING_RESISTANT
#define ECC_TIMING_RESISTANT
#define WC_RSA_BLINDING

#define HAVE_ECC
#define ECC_USER_CURVES         /* P-256 by default, plus: */
#define HAVE_ECC384
#define HAVE_ECC521
#define ECC_SHAMIR
#define HAVE_CURVE25519
#define CURVE25519_SMALL

#define WC_RSA_PSS
#define RSA_MIN_SIZE            2048

#define HAVE_AESGCM
#define GCM_SMALL
#define WOLFSSL_AES_128
#define WOLFSSL_AES_256
#define HAVE_AES_CBC
#define HAVE_CHACHA
#define HAVE_POLY1305
#define WOLFSSL_SHA384
#define WOLFSSL_SHA512
#define USE_SLOW_SHA256
#define USE_SLOW_SHA512

#define NO_DH
#define NO_DSA
#define NO_MD4
#define NO_MD5
#define NO_RC4
#define NO_DES3
#define NO_PWDBASED
#define NO_PKCS8
#define NO_PKCS12
#define WOLFSSL_NO_SHAKE128
#define WOLFSSL_NO_SHAKE256

#endif
