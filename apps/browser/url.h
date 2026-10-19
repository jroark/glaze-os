/*
 * URLs: RFC 3986 parsing, reference resolution (section 5.2) and
 * serialization. Paths, queries and fragments are kept percent-encoded;
 * parsing encodes spaces, controls and Latin-1 bytes (as UTF-8).
 */
#ifndef BROWSER_URL_H
#define BROWSER_URL_H

#include <stdbool.h>

#define URL_PART_MAX    1536
#define URL_HOST_MAX    128

struct url {
    char scheme[16];            /* lowercase, no ':' */
    bool has_auth, has_query, has_frag;
    char host[URL_HOST_MAX];    /* lowercase */
    int port;                   /* -1 when absent */
    char path[URL_PART_MAX];
    char query[URL_PART_MAX];
    char frag[256];
};

/* Parses an absolute URL or a relative reference (scheme[0] == 0). */
bool url_parse(const char *s, struct url *u);
/* Writes u; returns the length (truncated to n - 1). */
int url_format(const struct url *u, char *out, int n, bool with_frag);
/* Resolves ref against the absolute base: a malloc'd string, or NULL.
 * data: references are returned as they are, without a length limit. */
char *url_resolve(const char *base, const char *ref);
/* 80 for http and so on, or -1. */
int url_default_port(const char *scheme);
/* True when a and b differ only in their fragments. */
bool url_same_document(const char *a, const char *b);
/* Appends s, percent-encoded as application/x-www-form-urlencoded (Latin-1
 * text encoded as UTF-8), to out; returns the new length or -1 if full. */
int url_form_encode(char *out, int len, int max, const char *s);

#endif
