/*
 * Background TCP + TLS connections for IMAP and SMTP (glaze/net.h + wolfSSL).
 * DNS resolution, TCP connect, and TLS handshakes run on a background worker
 * thread per channel so the UI stays responsive; once CONN_UP is reached,
 * non-blocking reads and writes happen from the main thread.
 */

#include <glaze/fs.h>
#include <glaze/net.h>
#include <glaze/notify.h>
#include <glaze/res.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/random.h>

#include "gmail.h"
#include "mail.h"

#define WAIT_UP_MS      8000
#define RESOLVE_MS      10000
#define CONNECT_MS      15000
#define SEED_LEN        32

static struct gz_mutex lock = GZ_MUTEX_INIT;
static struct gz_mutex tls_lock = GZ_MUTEX_INIT;
static WOLFSSL_CTX *tls_ctx;
static bool tls_ctx_failed;

struct channel {
    int generation;
    int state;
    int sock;
    WOLFSSL *ssl;
    bool in_handshake;
    char error[96];
};

static struct channel chans[CONN_CH_COUNT] = {
    { .sock = -1, .state = CONN_NONE },
    { .sock = -1, .state = CONN_NONE },
};

struct attempt {
    int ch;
    int generation;
    char host[MAIL_HOST_MAX];
    uint16_t port;
    bool use_tls;
};

/* ---- wolfSSL RNG seed and RTC time callbacks ---------------------------- */

static uint8_t rng_pool[WC_SHA256_DIGEST_SIZE];
static bool rng_pool_ready;
static uint32_t rng_pool_count;

static void seed_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/data/seed", res_bundle());
}

static void collect_noise(uint8_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        gz_yield();
        uint64_t t = gz_time_us();
        buf[i] = (uint8_t)(t ^ (t >> 8) ^ (t >> 16));
    }
}

int glaze_tls_seed(unsigned char *out, unsigned int n)
{
    uint8_t buf[WC_SHA256_DIGEST_SIZE + 64 + 4];

    if (!rng_pool_ready) {
        uint8_t init[SEED_LEN + 256];
        char path[FS_PATH_MAX];
        memset(init, 0, SEED_LEN);
        seed_path(path, sizeof(path));
        fs_read_file(path, init, SEED_LEN);
        collect_noise(init + SEED_LEN, 256);
        wc_Sha256Hash(init, sizeof(init), rng_pool);
        rng_pool_ready = true;
    }
    while (n) {
        memcpy(buf, rng_pool, sizeof(rng_pool));
        collect_noise(buf + sizeof(rng_pool), 64);
        memcpy(buf + sizeof(rng_pool) + 64, &rng_pool_count, 4);
        rng_pool_count++;
        wc_Sha256Hash(buf, sizeof(buf), rng_pool);
        uint8_t block[WC_SHA256_DIGEST_SIZE];
        buf[0] ^= 0x5C;
        wc_Sha256Hash(buf, sizeof(buf), block);
        unsigned int k = n < sizeof(block) ? n : sizeof(block);
        memcpy(out, block, k);
        out += k;
        n -= k;
    }
    return 0;
}

time_t glaze_tls_time(time_t *t)
{
    struct gz_datetime dt;
    time_t now = 0;

    if (gz_rtc_get(&dt) >= 0)
        now = (time_t)((long long)gz_dt_to_secs(&dt) + 946684800LL);
    if (t)
        *t = now;
    return now;
}

static void save_seed(void)
{
    WC_RNG rng;
    uint8_t seed[SEED_LEN];
    char path[FS_PATH_MAX], dir[FS_PATH_MAX];

    memset(&rng, 0, sizeof(rng));
    if (wc_InitRng(&rng) != 0)
        return;
    if (wc_RNG_GenerateBlock(&rng, seed, sizeof(seed)) == 0) {
        seed_path(path, sizeof(path));
        if (fs_write_file(path, seed, sizeof(seed)) == GZ_ENOENT) {
            snprintf(dir, sizeof(dir), "%s/data", res_bundle());
            fs_mkdirs(dir);
            fs_write_file(path, seed, sizeof(seed));
        }
    }
    wc_FreeRng(&rng);
}

/* ---- wolfSSL context and I/O callbacks ---------------------------------- */

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
        len = (len << 8) | d[2 + i];
    len += 2u + (uint32_t)nb;
    return len <= avail ? len : 0;
}

static bool ensure_tls_ctx(void)
{
    uint32_t size = 0;
    const uint8_t *roots;
    int loaded = 0;

    if (tls_ctx)
        return true;
    if (tls_ctx_failed)
        return false;
    tls_ctx_failed = true;
    if (wolfSSL_Init() != WOLFSSL_SUCCESS)
        return false;
    tls_ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (!tls_ctx)
        return false;
    wolfSSL_CTX_set_verify(tls_ctx, WOLFSSL_VERIFY_PEER, NULL);
    roots = gz_romfs_map("ssl/roots.der", &size);
    for (uint32_t off = 0; roots && off < size;) {
        uint32_t n = der_len(roots + off, size - off);
        if (!n)
            break;
        if (wolfSSL_CTX_load_verify_buffer(tls_ctx, roots + off, n, WOLFSSL_FILETYPE_ASN1) == WOLFSSL_SUCCESS)
            loaded++;
        off += n;
    }
    if (!loaded) {
        wolfSSL_CTX_free(tls_ctx);
        tls_ctx = NULL;
        return false;
    }
    save_seed();
    tls_ctx_failed = false;
    return true;
}

static bool is_ip_addr(const char *host)
{
    for (; *host; host++)
        if ((*host < '0' || *host > '9') && *host != '.')
            return false;
    return true;
}

static int tls_io_recv(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    struct channel *c = ctx;
    uint32_t timeout = c->in_handshake ? CONNECT_MS : 0;
    int r = net_recv(c->sock, buf, (uint32_t)sz, timeout);

    (void)ssl;
    if (r > 0)
        return r;
    if (r == GZ_ETIMEDOUT)
        return c->in_handshake ? WOLFSSL_CBIO_ERR_GENERAL : WOLFSSL_CBIO_ERR_WANT_READ;
    if (r == 0)
        return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    return WOLFSSL_CBIO_ERR_CONN_RST;
}

static int tls_io_send(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    struct channel *c = ctx;
    int r = net_send(c->sock, buf, (uint32_t)sz);

    (void)ssl;
    if (r >= 0)
        return r;
    return WOLFSSL_CBIO_ERR_CONN_RST;
}

static const char *tls_err_msg(int code)
{
    switch (code) {
    case DOMAIN_NAME_MISMATCH:
    case IPADDR_MISMATCH:
        return "Server certificate name does not match the host.";
    case ASN_BEFORE_DATE_E:
    case ASN_AFTER_DATE_E:
        return "Certificate date check failed. Check Date & Time in Settings.";
    case ASN_NO_SIGNER_E:
    case ASN_SELF_SIGNED_E:
    case VERIFY_CERT_ERROR:
        return "Server certificate is not trusted.";
    default:
        return "TLS security handshake failed.";
    }
}

/* ---- Background connection worker --------------------------------------- */

static void finish(const struct attempt *a, int s, WOLFSSL *ssl, const char *why)
{
    gz_mutex_lock(&lock);
    struct channel *c = &chans[a->ch];
    if (a->generation != c->generation) {
        gz_mutex_unlock(&lock);
        if (ssl) {
            gz_mutex_lock(&tls_lock);
            wolfSSL_free(ssl);
            gz_mutex_unlock(&tls_lock);
        }
        if (s >= 0)
            net_close(s);
        return;
    }
    if (s >= 0) {
        c->sock = s;
        c->ssl = ssl;
        c->in_handshake = false;
        if (ssl) {
            wolfSSL_SetIOReadCtx(ssl, c);
            wolfSSL_SetIOWriteCtx(ssl, c);
        }
        c->state = CONN_UP;
    } else {
        strlcpy(c->error, why ? why : "Connection failed.", sizeof(c->error));
        c->state = CONN_FAILED;
    }
    gz_mutex_unlock(&lock);
}

static void connect_thread(void *arg)
{
    struct attempt *a = arg;
    struct net_status st;
    uint32_t addr = 0;
    int s = -1, rv;

    if (net_status(&st) < 0 || st.state == NET_NO_CARD) {
        finish(a, -1, NULL, "There is no network. Insert an Ethernet card.");
        free(a);
        return;
    }
    if (net_wait_up(WAIT_UP_MS) < 0) {
        finish(a, -1, NULL, "The network isn't ready yet.");
        free(a);
        return;
    }
    if (net_aton(a->host, &addr) < 0 && net_resolve(a->host, &addr, RESOLVE_MS) < 0) {
        finish(a, -1, NULL, "The mail server's address could not be resolved.");
        free(a);
        return;
    }
    if ((s = net_socket(NET_TCP)) < 0) {
        finish(a, -1, NULL, net_strerror(s));
        free(a);
        return;
    }
    if ((rv = net_connect(s, addr, a->port, CONNECT_MS)) < 0) {
        net_close(s);
        finish(a, -1, NULL, rv == GZ_ETIMEDOUT ? "The mail server didn't answer." : "The server refused the connection.");
        free(a);
        return;
    }
    net_sockopt(s, NET_OPT_NODELAY, 1);

    if (!a->use_tls) {
        finish(a, s, NULL, NULL);
        free(a);
        return;
    }

    gz_mutex_lock(&tls_lock);
    if (!ensure_tls_ctx()) {
        gz_mutex_unlock(&tls_lock);
        net_close(s);
        finish(a, -1, NULL, "Could not load trusted SSL root certificates.");
        free(a);
        return;
    }
    WOLFSSL *ssl = wolfSSL_new(tls_ctx);
    if (!ssl) {
        gz_mutex_unlock(&tls_lock);
        net_close(s);
        finish(a, -1, NULL, "Out of memory starting TLS.");
        free(a);
        return;
    }
    bool ip = is_ip_addr(a->host);
    if ((ip ? wolfSSL_check_ip_address(ssl, a->host) : wolfSSL_check_domain_name(ssl, a->host)) != WOLFSSL_SUCCESS ||
        (!ip && wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, a->host, (unsigned short)strlen(a->host)) != WOLFSSL_SUCCESS)) {
        wolfSSL_free(ssl);
        gz_mutex_unlock(&tls_lock);
        net_close(s);
        finish(a, -1, NULL, "Could not configure TLS host verification.");
        free(a);
        return;
    }

    struct channel hs_chan = { .sock = s, .in_handshake = true };
    wolfSSL_SSLSetIORecv(ssl, tls_io_recv);
    wolfSSL_SSLSetIOSend(ssl, tls_io_send);
    wolfSSL_SetIOReadCtx(ssl, &hs_chan);
    wolfSSL_SetIOWriteCtx(ssl, &hs_chan);

    int tret = wolfSSL_connect(ssl);
    if (tret != WOLFSSL_SUCCESS) {
        int err = wolfSSL_get_error(ssl, tret);
        const char *why = tls_err_msg(err);
        wolfSSL_free(ssl);
        gz_mutex_unlock(&tls_lock);
        net_close(s);
        finish(a, -1, NULL, why);
        free(a);
        return;
    }
    gz_mutex_unlock(&tls_lock);
    finish(a, s, ssl, NULL);
    free(a);
}

void conn_start(int ch, const char *host, uint16_t port, bool use_tls)
{
    if (ch < 0 || ch >= CONN_CH_COUNT)
        return;
    conn_close(ch);

    struct attempt *a = malloc(sizeof(*a));
    gz_mutex_lock(&lock);
    struct channel *c = &chans[ch];
    c->state = CONN_CONNECTING;
    c->error[0] = 0;
    if (!a) {
        strlcpy(c->error, "Out of memory.", sizeof(c->error));
        c->state = CONN_FAILED;
        gz_mutex_unlock(&lock);
        return;
    }
    a->ch = ch;
    a->generation = c->generation;
    strlcpy(a->host, host, sizeof(a->host));
    a->port = port;
    a->use_tls = use_tls;
    gz_mutex_unlock(&lock);

    if (gz_thread_create(connect_thread, a) < 0) {
        free(a);
        gz_mutex_lock(&lock);
        strlcpy(c->error, "Could not start connection thread.", sizeof(c->error));
        c->state = CONN_FAILED;
        gz_mutex_unlock(&lock);
    }
}

int conn_state(int ch)
{
    if (ch < 0 || ch >= CONN_CH_COUNT)
        return CONN_NONE;
    gz_mutex_lock(&lock);
    int s = chans[ch].state;
    gz_mutex_unlock(&lock);
    return s;
}

const char *conn_error(int ch)
{
    if (ch < 0 || ch >= CONN_CH_COUNT)
        return "";
    return chans[ch].error;
}

int conn_send(int ch, const void *buf, size_t len)
{
    if (ch < 0 || ch >= CONN_CH_COUNT)
        return -1;
    struct channel *c = &chans[ch];
    if (c->sock < 0)
        return -1;
    if (c->ssl) {
        const uint8_t *p = buf;
        size_t rem = len;
        while (rem > 0) {
            gz_mutex_lock(&tls_lock);
            int r = wolfSSL_write(c->ssl, p, (int)rem);
            int err = r <= 0 ? wolfSSL_get_error(c->ssl, r) : 0;
            gz_mutex_unlock(&tls_lock);
            if (r > 0) {
                p += r;
                rem -= (size_t)r;
                continue;
            }
            if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE)
                continue;
            return -1;
        }
        return 0;
    }
    int n = net_send(c->sock, buf, (uint32_t)len);
    return n == (int)len ? 0 : -1;
}

int conn_recv(int ch, void *buf, size_t len)
{
    if (ch < 0 || ch >= CONN_CH_COUNT)
        return -1;
    struct channel *c = &chans[ch];
    if (c->sock < 0)
        return -1;
    if (c->ssl) {
        gz_mutex_lock(&tls_lock);
        int r = wolfSSL_read(c->ssl, buf, (int)len);
        int err = r <= 0 ? wolfSSL_get_error(c->ssl, r) : 0;
        gz_mutex_unlock(&tls_lock);
        if (r > 0)
            return r;
        if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE)
            return 0;
        return -1;
    }
    int n = net_recv(c->sock, buf, (uint32_t)len, 0);
    if (n == GZ_ETIMEDOUT)
        return 0;
    return n > 0 ? n : -1;
}

void conn_close(int ch)
{
    if (ch < 0 || ch >= CONN_CH_COUNT)
        return;
    gz_mutex_lock(&lock);
    struct channel *c = &chans[ch];
    c->generation++;
    WOLFSSL *ssl = c->ssl;
    int s = c->sock;
    c->ssl = NULL;
    c->sock = -1;
    c->state = CONN_NONE;
    gz_mutex_unlock(&lock);

    if (ssl) {
        gz_mutex_lock(&tls_lock);
        wolfSSL_free(ssl);
        gz_mutex_unlock(&tls_lock);
    }
    if (s >= 0)
        net_close(s);
}
