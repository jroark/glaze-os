/*
 * TLS for https: one wolfSSL client context, made on the first secure
 * fetch so that start-up and plain-http browsing don't pay for parsing the
 * root certificates. Servers are verified against those roots (Mozilla's,
 * romfs ssl/roots.der), by host name and by date. Recent sessions are kept
 * per host so a page's images resume instead of repeating the handshake.
 *
 * wolfSSL's random generator is seeded here (glaze_tls_seed) from a seed
 * saved by the previous run plus timing noise, and gets the time of day
 * from the platform (glaze_tls_time); see apps/browser/tls/user_settings.h.
 */

#include "net.h"
#include "plat.h"
#include "web.h"

#include <stdlib.h>
#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/random.h>

#define SESSIONS    8

static WOLFSSL_CTX *ctx;
static bool ctx_failed;

static struct {
    char *host;
    int port;
    WOLFSSL_SESSION *s;
    uint32_t used;
} sessions[SESSIONS];
static uint32_t session_clock;

/* ---- randomness and time -------------------------------------------------------- */

static uint8_t pool[WC_SHA256_DIGEST_SIZE];
static bool pool_ready;
static uint32_t pool_count;

/* Seed material for the DRBG: SHA-256 output keyed by a pool of the saved
 * seed and noise, with fresh noise mixed into every request. */
/* Shared wolfSSL resolves these callbacks from the executable. Browser
 * objects otherwise use hidden visibility for the JavaScript engine. */
__attribute__((visibility("default")))
int glaze_tls_seed(unsigned char *out, unsigned int n)
{
    uint8_t buf[WC_SHA256_DIGEST_SIZE + 64 + 4];

    if (!pool_ready) {
        uint8_t init[PLAT_SEED_LEN + 256];
        memset(init, 0, PLAT_SEED_LEN);
        plat_load_seed(init);
        plat_noise(init + PLAT_SEED_LEN, 256);
        wc_Sha256Hash(init, sizeof(init), pool);
        pool_ready = true;
    }
    while (n) {
        memcpy(buf, pool, sizeof(pool));
        plat_noise(buf + sizeof(pool), 64);
        memcpy(buf + sizeof(pool) + 64, &pool_count, 4);
        pool_count++;
        wc_Sha256Hash(buf, sizeof(buf), pool);      /* the pool moves on */
        uint8_t block[WC_SHA256_DIGEST_SIZE];
        buf[0] ^= 0x5C;                             /* output differs from the pool */
        wc_Sha256Hash(buf, sizeof(buf), block);
        unsigned int k = n < sizeof(block) ? n : sizeof(block);
        memcpy(out, block, k);
        out += k;
        n -= k;
    }
    return 0;
}

__attribute__((visibility("default")))
time_t glaze_tls_time(time_t *t)
{
    time_t now = (time_t)plat_time();

    if (t)
        *t = now;
    return now;
}

/* Saves a seed for the next run, drawn from a generator seeded now. */
static void save_seed(void)
{
    WC_RNG rng;
    uint8_t seed[PLAT_SEED_LEN];

    memset(&rng, 0, sizeof(rng));
    if (wc_InitRng(&rng) != 0)
        return;
    if (wc_RNG_GenerateBlock(&rng, seed, sizeof(seed)) == 0)
        plat_save_seed(seed);
    wc_FreeRng(&rng);
}

/* ---- the context ------------------------------------------------------------------ */

/* Length of the DER element at d (a certificate), or 0. */
static uint32_t der_len(const uint8_t *d, uint32_t avail)
{
    if (avail < 2 || d[0] != 0x30)
        return 0;
    if (d[1] < 0x80)
        return 2u + d[1] <= avail ? 2u + d[1] : 0;
    int nb = d[1] & 0x7F;
    if (nb < 1 || nb > 3 || avail < 2u + (uint32_t)nb)
        return 0;
    uint32_t len = 0;
    for (int i = 0; i < nb; i++)
        len = len << 8 | d[2 + i];
    len += 2u + (uint32_t)nb;
    return len <= avail ? len : 0;
}

static bool make_ctx(void)
{
    uint32_t size;
    const uint8_t *roots;
    int loaded = 0;

    if (ctx)
        return true;
    if (ctx_failed)
        return false;
    ctx_failed = true;
    if (wolfSSL_Init() != WOLFSSL_SUCCESS)
        return false;
    ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (!ctx)
        return false;
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, NULL);
    roots = plat_roots(&size);
    for (uint32_t off = 0; roots && off < size;) {
        uint32_t n = der_len(roots + off, size - off);
        if (!n)
            break;
        /* A root wolfSSL can't use (an unsupported key, say) is skipped. */
        if (wolfSSL_CTX_load_verify_buffer(ctx, roots + off, n, WOLFSSL_FILETYPE_ASN1) == WOLFSSL_SUCCESS)
            loaded++;
        off += n;
    }
    if (!loaded) {
        wolfSSL_CTX_free(ctx);
        ctx = NULL;
        return false;
    }
    save_seed();
    ctx_failed = false;
    return true;
}

static bool is_ip(const char *host)
{
    for (; *host; host++)
        if ((*host < '0' || *host > '9') && *host != '.')
            return false;
    return true;
}

WOLFSSL *tls_new(const char *host, int port, int *err)
{
    WOLFSSL *ssl;

    *err = WE_NOMEM;
    if (!make_ctx()) {
        *err = WE_TLS;
        return NULL;
    }
    ssl = wolfSSL_new(ctx);
    if (!ssl)
        return NULL;
    /* An address is checked against the certificate's IP names; other
     * hosts must be proper domain names (or "localhost") to check at all. */
    bool ip = is_ip(host);
    if ((ip ? wolfSSL_check_ip_address(ssl, host) : wolfSSL_check_domain_name(ssl, host)) != WOLFSSL_SUCCESS) {
        *err = wolfSSL_get_error(ssl, 0) == MEMORY_ERROR ? WE_NOMEM : WE_CERT_NAME;
        wolfSSL_free(ssl);
        return NULL;
    }
    if (!ip && wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, (unsigned short)strlen(host)) != WOLFSSL_SUCCESS) {
        wolfSSL_free(ssl);
        return NULL;
    }
    for (int i = 0; i < SESSIONS; i++) {
        if (sessions[i].s && sessions[i].port == port && !strcmp(sessions[i].host, host)) {
            sessions[i].used = ++session_clock;
            wolfSSL_set_session(ssl, sessions[i].s);
            break;
        }
    }
    return ssl;
}

void tls_free(WOLFSSL *ssl, const char *host, int port)
{
    WOLFSSL_SESSION *s;

    if (host && (s = wolfSSL_get1_session(ssl))) {
        int slot = 0;
        for (int i = 0; i < SESSIONS; i++) {
            if (sessions[i].s && sessions[i].port == port && !strcmp(sessions[i].host, host)) {
                slot = i;
                break;
            }
            if (!sessions[i].s || sessions[i].used < sessions[slot].used)
                slot = i;
        }
        char *h = strdup(host);
        if (h) {
            if (sessions[slot].s)
                wolfSSL_SESSION_free(sessions[slot].s);
            free(sessions[slot].host);
            sessions[slot].host = h;
            sessions[slot].port = port;
            sessions[slot].s = s;
            sessions[slot].used = ++session_clock;
        } else {
            free(h);
            wolfSSL_SESSION_free(s);
        }
    }
    wolfSSL_free(ssl);
}

int tls_error(int code)
{
    switch (code) {
    case DOMAIN_NAME_MISMATCH:
    case IPADDR_MISMATCH:
        return WE_CERT_NAME;
    case ASN_BEFORE_DATE_E:
    case ASN_AFTER_DATE_E:
        return WE_CERT_DATE;
    case ASN_NO_SIGNER_E:
    case ASN_SELF_SIGNED_E:
    case ASN_SIG_CONFIRM_E:
    case ASN_SIG_KEY_E:
    case ASN_NAME_INVALID_E:
    case ASN_PATHLEN_SIZE_E:
    case ASN_PATHLEN_INV_E:
    case SIG_VERIFY_E:
    case VERIFY_CERT_ERROR:
    case KEYUSE_SIGNATURE_E:
    case KEYUSE_ENCIPHER_E:
    case EXTKEYUSE_AUTH_E:
    case WOLFSSL_BAD_CERTTYPE:
        return WE_CERT;
    case MEMORY_E:
        return WE_NOMEM;
    case SOCKET_ERROR_E:
    case SOCKET_PEER_CLOSED_E:
        return WE_RESET;
    default:
        return WE_TLS;
    }
}
