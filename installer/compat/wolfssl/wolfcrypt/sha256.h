/* Installer-only adapter: retain the production writer's hash interface. */
#pragma once
#include <stdint.h>
typedef uint32_t word32;
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
typedef struct { BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE hash; } wc_Sha256;
static inline int wc_InitSha256(wc_Sha256 *s) {
    if (BCryptOpenAlgorithmProvider(&s->alg,BCRYPT_SHA256_ALGORITHM,NULL,0)) return -1;
    return BCryptCreateHash(s->alg,&s->hash,NULL,0,NULL,0,0) ? -1 : 0;
}
static inline int wc_Sha256Update(wc_Sha256 *s,const unsigned char *p,word32 n) { return BCryptHashData(s->hash,(PUCHAR)p,n,0) ? -1 : 0; }
static inline int wc_Sha256Final(wc_Sha256 *s,unsigned char *p) { return BCryptFinishHash(s->hash,p,32,0) ? -1 : 0; }
static inline void wc_Sha256Free(wc_Sha256 *s) { if(s->hash) BCryptDestroyHash(s->hash); if(s->alg) BCryptCloseAlgorithmProvider(s->alg,0); }
#else
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA256_CTX wc_Sha256;
static inline int wc_InitSha256(wc_Sha256 *s) { return !CC_SHA256_Init(s); }
static inline int wc_Sha256Update(wc_Sha256 *s,const unsigned char *p,word32 n) { return !CC_SHA256_Update(s,p,n); }
static inline int wc_Sha256Final(wc_Sha256 *s,unsigned char *p) { return !CC_SHA256_Final(p,s); }
static inline void wc_Sha256Free(wc_Sha256 *s) { (void)s; }
#endif
