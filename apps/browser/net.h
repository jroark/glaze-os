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

struct conn {
    int fd;
    bool connected;
};

int conn_open(struct conn *c, uint32_t addr, int port);    /* 0 or WE_* */
int conn_ready(struct conn *c);         /* 1 connected, 0 pending, WE_* */
int conn_read(struct conn *c, void *buf, int n);    /* bytes, 0 at EOF, -CONN_AGAIN, WE_* */
int conn_write(struct conn *c, const void *buf, int n);
void conn_close(struct conn *c);

/* ---- HTTP ------------------------------------------------------------------- */

struct http;

/* GET (post == NULL) or POST of url; cookies may be NULL. */
struct http *http_start(const char *url, const char *post, int post_len, const char *post_type,
                        const char *cookies, int max_body);
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
