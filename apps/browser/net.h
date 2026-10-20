/*
 * The browser's network side: non-blocking TCP connections (BSD sockets),
 * an HTTP/1.1 client, and the loader that schedules fetches.
 */
#ifndef BROWSER_NET_H
#define BROWSER_NET_H

#include <stdbool.h>
#include <stdint.h>

/* ---- connections ------------------------------------------------------- */

#define CONN_AGAIN  1           /* conn_read/conn_write: try later */

struct WOLFSSL;

struct conn {
    int fd;
    bool connected;             /* TCP, and the TLS handshake if any */
    bool tcp_up;
    int err;                    /* errno of the last socket failure under TLS */
    struct WOLFSSL *tls;        /* NULL for plain TCP */
    char *host;                 /* TLS: the server name, malloc'd */
    int port;
};

/* tls_host is the name to verify the server's certificate against, or NULL
 * for plain TCP. */
int conn_open(struct conn *c, uint32_t addr, int port, const char *tls_host);  /* 0 or WE_* */
int conn_ready(struct conn *c);         /* 1 connected, 0 pending, WE_* */
int conn_read(struct conn *c, void *buf, int n);    /* bytes, 0 at EOF, -CONN_AGAIN, WE_* */
int conn_write(struct conn *c, const void *buf, int n);
void conn_close(struct conn *c);

/* ---- TLS (tls.c, wolfSSL) ------------------------------------------------------ */

/* A new client session for host (SNI, certificate name), resuming a recent
 * session with host:port when there is one; NULL with *err set to WE_*. */
struct WOLFSSL *tls_new(const char *host, int port, int *err);
/* Keeps the session of a finished connection for resumption, then frees it. */
void tls_free(struct WOLFSSL *ssl, const char *host, int port);
/* WE_* for a failed handshake, read or write (wolfSSL's error code). */
int tls_error(int code);

/* ---- HTTP ------------------------------------------------------------------- */

struct http;

/* GET (post == NULL) or POST of url; cookies may be NULL. */
struct http *http_start(const char *url, const char *post, int post_len, const char *post_type,
                        const char *cookies, int max_body);
/* A bounded, identity-encoded GET streamed to sink. Nonzero sink result
 * aborts the fetch. The callback receives decoded HTTP transfer bytes. */
struct http *http_start_stream(const char *url, int max_body,
                               int (*sink)(const uint8_t *, int, void *), void *arg);
int http_step(struct http *h);          /* 1 done, 0 pending, WE_* */
int http_status(const struct http *h);
/* The n-th header with this name (case-insensitive), or NULL. */
const char *http_header(const struct http *h, const char *name, int n);
/* Takes the (decoded) body; the caller frees it. */
uint8_t *http_take_body(struct http *h, int *len);
void http_progress(const struct http *h, int *got, int *total);
void http_free(struct http *h);

/* Inflate gzip / zlib data (in image.c, from stb_image). */
uint8_t *web_inflate(const uint8_t *in, int len, bool zlib_header, int *out_len);

/* ---- loader ------------------------------------------------------------------ */

enum { F_DOC, F_CSS, F_IMG };

struct fetch_result {
    int id, kind, tag;
    int err;                    /* 0 or WE_* */
    int status;                 /* HTTP status (200 for local schemes) */
    uint8_t *data;              /* malloc'd; the receiver frees it */
    int len;
    char *ctype;                /* malloc'd or NULL */
    char *url;                  /* final URL after redirects, malloc'd */
};

/* Queues a fetch; tag is the caller's (image slot, sheet index). */
int ld_start(const char *url, int kind, int tag, const char *post, const char *post_type, bool reload);
void ld_cancel(int id);
void ld_cancel_all(void);
bool ld_busy(void);
/* Advances fetches for up to budget_ms. */
void ld_step(int budget_ms);
/* A finished fetch, if any. */
bool ld_take(struct fetch_result *r);
/* Bytes received and expected (0 if unknown) for one fetch. */
void ld_progress(int id, int *got, int *total);
void ld_free_result(struct fetch_result *r);

#endif
