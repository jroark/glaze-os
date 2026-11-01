/*
 * wolfCrypt configuration for the kernel (kernel/ota.c, docs/os/OTA.md): only
 * Ed25519 signature verification and SHA-256/SHA-512, with no heap, RNG,
 * threads, file system or time. The same file configures the native
 * build in host/ (make -C host ota-test).
 */
#ifndef GLAZE_KERNEL_WOLFSSL_USER_SETTINGS_H
#define GLAZE_KERNEL_WOLFSSL_USER_SETTINGS_H

#define WOLFCRYPT_ONLY
#define SINGLE_THREADED
#define NO_FILESYSTEM
#define NO_ERROR_STRINGS
#define NO_MAIN_DRIVER
#define WOLFSSL_NO_ASM
#define NO_DEV_RANDOM
#define WC_NO_RNG
#define NO_ASN_TIME
#define WOLFSSL_NO_MALLOC
#define WOLFSSL_NO_FENCE

#if !__STDC_HOSTED__
#undef __unix__
#undef __linux__
#define SIZEOF_LONG             4
#define SIZEOF_LONG_LONG        8
#define WOLFSSL_GENERAL_ALIGNMENT 4
#define WOLFSSL_USE_ALIGN       /* lw/sw fault on unaligned addresses */
#define NO_STDIO_FILESYSTEM
#define NO_INLINE_ASM_CHECKS
#define STRING_USER             /* the kernel's string.c, no <stdlib.h> */
#include <string.h>
#define XMEMCPY(d, s, l)        memcpy((d), (s), (l))
#define XMEMSET(b, c, l)        memset((b), (c), (l))
#define XMEMCMP(s1, s2, n)      memcmp((s1), (s2), (n))
#define XMEMMOVE(d, s, l)       memmove((d), (s), (l))
#define XSTRLEN(s1)             strlen((s1))
#define XSTRNCPY(s1, s2, n)     strncpy((s1), (s2), (n))
#define XSTRNCMP(s1, s2, n)     strncmp((s1), (s2), (n))
#endif

#define HAVE_ED25519
#define HAVE_ED25519_VERIFY
#define ED25519_SMALL
#define WOLFSSL_SHA512
#define USE_SLOW_SHA256
#define USE_SLOW_SHA512

#define NO_RSA
#define NO_DH
#define NO_DSA
#define NO_MD4
#define NO_MD5
#define NO_SHA
#define NO_RC4
#define NO_DES3
#define NO_AES
#define NO_HMAC
#define NO_PWDBASED
#define NO_PKCS8
#define NO_PKCS12
#define NO_CODING
#define NO_ASN
#define NO_SIG_WRAPPER
#define WOLFSSL_NO_SHAKE128
#define WOLFSSL_NO_SHAKE256
#define WOLFSSL_NOSHA512_224
#define WOLFSSL_NOSHA512_256

#endif
