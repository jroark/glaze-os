/* URL parsing and RFC 3986 reference resolution. */

#include "url.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char hexd[] = "0123456789ABCDEF";

/* Where to continue an snprintf chain into a buffer of n bytes. */
#define MIN_LEN(len, n) ((len) < (n) ? (len) : (n) - 1)

int url_default_port(const char *scheme)
{
    if (!strcmp(scheme, "http"))
        return 80;
    if (!strcmp(scheme, "https"))
        return 443;
    return -1;
}

/* Copy [s, s+n) into out, percent-encoding what may not appear raw. */
static bool put_encoded(char *out, int max, const char *s, int n)
{
    int o = 0;

    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t' || c == '\n' || c == '\r')
            continue;               /* browsers drop these */
        if (c <= 0x20 || c == '"' || c == '<' || c == '>' || c == '`' || c == 0x7F) {
            if (o + 3 >= max)
                return false;
            out[o++] = '%';
            out[o++] = hexd[c >> 4];
            out[o++] = hexd[c & 15];
        } else if (c >= 0x80) {
            /* Latin-1 to UTF-8, then percent-encoded. */
            if (o + 6 >= max)
                return false;
            unsigned b1 = 0xC0 | c >> 6, b2 = 0x80 | (c & 0x3F);
            out[o++] = '%';
            out[o++] = hexd[b1 >> 4];
            out[o++] = hexd[b1 & 15];
            out[o++] = '%';
            out[o++] = hexd[b2 >> 4];
            out[o++] = hexd[b2 & 15];
        } else {
            if (o + 1 >= max)
                return false;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return true;
}

bool url_parse(const char *s, struct url *u)
{
    memset(u, 0, sizeof(*u));
    u->port = -1;

    while (*s && (unsigned char)*s <= ' ')
        s++;
    int len = (int)strlen(s);
    while (len && (unsigned char)s[len - 1] <= ' ')
        len--;
    const char *end = s + len, *p = s;

    /* scheme */
    if (isalpha((unsigned char)*p)) {
        const char *q = p;
        while (q < end && (isalnum((unsigned char)*q) || *q == '+' || *q == '-' || *q == '.'))
            q++;
        if (q < end && *q == ':') {
            if (q - p >= (int)sizeof(u->scheme))
                return false;
            for (int i = 0; i < q - p; i++)
                u->scheme[i] = (char)tolower((unsigned char)p[i]);
            p = q + 1;
        }
    }
    bool web = !strcmp(u->scheme, "http") || !strcmp(u->scheme, "https") || !u->scheme[0];
    /* Like browsers, treat backslashes in http(s) URLs as slashes. */
    char tmp[URL_PART_MAX * 2];
    if (web && memchr(p, '\\', (size_t)(end - p))) {
        int n = (int)(end - p);
        if (n >= (int)sizeof(tmp))
            return false;
        for (int i = 0; i < n; i++)
            tmp[i] = p[i] == '\\' ? '/' : p[i];
        tmp[n] = 0;
        p = tmp;
        end = tmp + n;
    }

    /* fragment, query */
    const char *hash = memchr(p, '#', (size_t)(end - p));
    const char *body_end = hash ? hash : end;
    if (hash) {
        u->has_frag = true;
        if (!put_encoded(u->frag, sizeof(u->frag), hash + 1, (int)(end - hash - 1)))
            u->frag[0] = 0;         /* an oversized fragment just goes */
    }
    const char *qm = memchr(p, '?', (size_t)(body_end - p));
    const char *path_end = qm ? qm : body_end;
    if (qm) {
        u->has_query = true;
        if (!put_encoded(u->query, sizeof(u->query), qm + 1, (int)(body_end - qm - 1)))
            return false;
    }

    /* authority */
    if (path_end - p >= 2 && p[0] == '/' && p[1] == '/') {
        u->has_auth = true;
        p += 2;
        const char *a_end = p;
        while (a_end < path_end && *a_end != '/')
            a_end++;
        const char *at = NULL;
        for (const char *q = p; q < a_end; q++)
            if (*q == '@')
                at = q;
        if (at)
            p = at + 1;             /* credentials are dropped */
        const char *colon = NULL;
        const char *h_end = a_end;
        if (*p == '[') {            /* IPv6 literal */
            const char *rb = memchr(p, ']', (size_t)(a_end - p));
            if (!rb)
                return false;
            h_end = rb + 1;
            if (h_end < a_end && *h_end == ':')
                colon = h_end;
        } else {
            colon = memchr(p, ':', (size_t)(a_end - p));
            if (colon)
                h_end = colon;
        }
        if (h_end - p >= URL_HOST_MAX)
            return false;
        for (int i = 0; i < h_end - p; i++)
            u->host[i] = (char)tolower((unsigned char)p[i]);
        if (colon && colon + 1 < a_end) {
            int port = 0;
            for (const char *q = colon + 1; q < a_end; q++) {
                if (!isdigit((unsigned char)*q))
                    return false;
                port = port * 10 + (*q - '0');
                if (port > 65535)
                    return false;
            }
            u->port = port;
        }
        p = a_end;
    }
    if (!put_encoded(u->path, sizeof(u->path), p, (int)(path_end - p)))
        return false;
    return true;
}

int url_format(const struct url *u, char *out, int n, bool with_frag)
{
    int len;

    if (u->has_auth) {
        len = snprintf(out, (size_t)n, "%s%s//%s", u->scheme, u->scheme[0] ? ":" : "", u->host);
        if (u->port >= 0 && u->port != url_default_port(u->scheme))
            len += snprintf(out + MIN_LEN(len, n), (size_t)(n - MIN_LEN(len, n)), ":%d", u->port);
    } else {
        len = snprintf(out, (size_t)n, "%s%s", u->scheme, u->scheme[0] ? ":" : "");
    }
    len += snprintf(out + MIN_LEN(len, n), (size_t)(n - MIN_LEN(len, n)), "%s", u->path);
    if (u->has_query)
        len += snprintf(out + MIN_LEN(len, n), (size_t)(n - MIN_LEN(len, n)), "?%s", u->query);
    if (with_frag && u->has_frag)
        len += snprintf(out + MIN_LEN(len, n), (size_t)(n - MIN_LEN(len, n)), "#%s", u->frag);
    return len < n ? len : n - 1;
}

/* RFC 3986 section 5.2.4, in place. */
static void remove_dot_segments(char *path)
{
    char out[URL_PART_MAX];
    int o = 0;
    const char *in = path;

    while (*in) {
        if (!strncmp(in, "../", 3)) {
            in += 3;
        } else if (!strncmp(in, "./", 2)) {
            in += 2;
        } else if (!strncmp(in, "/./", 3)) {
            in += 2;
        } else if (!strcmp(in, "/.")) {
            in = "/";               /* "/." becomes "/" */
        } else if (!strncmp(in, "/../", 4) || !strcmp(in, "/..")) {
            in += 3;
            if (!*in)
                in = "/";
            while (o > 0 && out[--o] != '/')
                ;
        } else if (!strcmp(in, ".") || !strcmp(in, "..")) {
            break;
        } else {
            do {
                if (o < URL_PART_MAX - 1)
                    out[o++] = *in;
                in++;
            } while (*in && *in != '/');
        }
    }
    out[o] = 0;
    memcpy(path, out, (size_t)o + 1);
}

char *url_resolve(const char *base, const char *ref)
{
    while (*ref && (unsigned char)*ref <= ' ')
        ref++;
    if (!strncasecmp(ref, "data:", 5))
        return strdup(ref);

    static struct url b, r, t;
    if (!url_parse(ref, &r))
        return NULL;
    if (!r.scheme[0] && (!base || !url_parse(base, &b) || !b.scheme[0]))
        return NULL;

    memset(&t, 0, sizeof(t));
    t.port = -1;
    if (r.scheme[0]) {
        t = r;
        remove_dot_segments(t.path);
    } else {
        if (r.has_auth) {
            t.has_auth = true;
            memcpy(t.host, r.host, sizeof(t.host));
            t.port = r.port;
            memcpy(t.path, r.path, sizeof(t.path));
            remove_dot_segments(t.path);
            t.has_query = r.has_query;
            memcpy(t.query, r.query, sizeof(t.query));
        } else {
            if (!r.path[0]) {
                memcpy(t.path, b.path, sizeof(t.path));
                t.has_query = r.has_query || b.has_query;
                memcpy(t.query, r.has_query ? r.query : b.query, sizeof(t.query));
            } else {
                if (r.path[0] == '/') {
                    memcpy(t.path, r.path, sizeof(t.path));
                } else if (b.has_auth && !b.path[0]) {
                    snprintf(t.path, sizeof(t.path), "/%s", r.path);
                } else {
                    char *slash = strrchr(b.path, '/');
                    int keep = slash ? (int)(slash - b.path) + 1 : 0;
                    snprintf(t.path, sizeof(t.path), "%.*s%s", keep, b.path, r.path);
                }
                remove_dot_segments(t.path);
                t.has_query = r.has_query;
                memcpy(t.query, r.query, sizeof(t.query));
            }
            t.has_auth = b.has_auth;
            memcpy(t.host, b.host, sizeof(t.host));
            t.port = b.port;
        }
        memcpy(t.scheme, b.scheme, sizeof(t.scheme));
    }
    t.has_frag = r.has_frag;
    memcpy(t.frag, r.frag, sizeof(t.frag));
    if (t.has_auth && !t.path[0] && url_default_port(t.scheme) > 0)
        strcpy(t.path, "/");

    int cap = URL_PART_MAX * 2 + URL_HOST_MAX + 320;
    char *out = malloc((size_t)cap);
    if (out)
        url_format(&t, out, cap, true);
    return out;
}

bool url_same_document(const char *a, const char *b)
{
    size_t la = strcspn(a, "#"), lb = strcspn(b, "#");

    return la == lb && !strncmp(a, b, la);
}

int url_form_encode(char *out, int len, int max, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == 0xA0)
            c = ' ';
        if (isalnum(c) || c == '*' || c == '-' || c == '.' || c == '_') {
            if (len + 1 >= max)
                return -1;
            out[len++] = (char)c;
        } else if (c == ' ') {
            if (len + 1 >= max)
                return -1;
            out[len++] = '+';
        } else if (c >= 0x80) {
            if (len + 6 >= max)
                return -1;
            unsigned b1 = 0xC0 | c >> 6, b2 = 0x80 | (c & 0x3F);
            out[len++] = '%';
            out[len++] = hexd[b1 >> 4];
            out[len++] = hexd[b1 & 15];
            out[len++] = '%';
            out[len++] = hexd[b2 >> 4];
            out[len++] = hexd[b2 & 15];
        } else {
            if (len + 3 >= max)
                return -1;
            out[len++] = '%';
            out[len++] = hexd[c >> 4];
            out[len++] = hexd[c & 15];
        }
    }
    out[len] = 0;
    return len;
}

/*
 * What the user typed in the address bar, as a URL (malloc'd). An address is
 * anything with a scheme, a host that looks like one ("example.org", an IPv4
 * address, "localhost", "router:8080") or a path ("intranet/wiki"); anything
 * else, and anything with a space in it, is a search. The search words are
 * form-encoded: "tom & jerry" sent as written lost everything from the "&".
 */
char *url_from_input(const char *text)
{
    static const char search[] = "http://frogfind.com/?q=";
    static char buf[URL_PART_MAX * 3 + sizeof(search)], t[URL_PART_MAX];

    while (*text == ' ')
        text++;
    size_t n = strlen(text);
    while (n && text[n - 1] == ' ')
        n--;
    if (!n || n >= URL_PART_MAX)
        return NULL;
    memcpy(t, text, n);
    t[n] = 0;

    bool space = strchr(t, ' ') != NULL;
    size_t scheme = 0;
    while (isalnum((unsigned char)t[scheme]) || (scheme && (t[scheme] == '+' || t[scheme] == '-' || t[scheme] == '.')))
        scheme++;
    /* "http://...", and the schemes with no "//": but not "host:8080". */
    if (scheme && t[scheme] == ':' && !strncmp(t + scheme, "://", 3)) {
        /* An address pasted with spaces in it keeps them, encoded. */
        size_t o = 0;
        for (size_t i = 0; t[i] && o + 4 < sizeof(buf); i++)
            if (t[i] == ' ') {
                memcpy(buf + o, "%20", 3);
                o += 3;
            } else {
                buf[o++] = t[i];
            }
        buf[o] = 0;
        return strdup(buf);
    }
    if (scheme && t[scheme] == ':' && !space &&
        (!strncasecmp(t, "about:", 6) || !strncasecmp(t, "res:", 4) || !strncasecmp(t, "data:", 5) ||
         !strncasecmp(t, "file:", 5) || !strncasecmp(t, "javascript:", 11)))
        return strdup(t);

    size_t host = strcspn(t, "/?#");
    bool address = !space && host > 0;
    if (address) {
        /* host[:port], the host with a dot and a name or number after the
         * last one, or one word followed by a path or a port. */
        size_t colon = host;
        for (size_t i = 0; i < host; i++)
            if (t[i] == ':') {
                colon = i;
                break;
            }
        bool port = colon < host;
        for (size_t i = colon + 1; i < host; i++)
            port = port && isdigit((unsigned char)t[i]);
        if (colon < host && (!port || colon + 1 == host || !colon))
            address = false;
        size_t dot = colon;
        while (dot && t[dot - 1] != '.')
            dot--;
        bool dotted = dot > 1, alpha = colon - dot >= 2, digits = colon > dot, ip = true;
        for (size_t i = dot; i < colon; i++) {
            alpha = alpha && isalpha((unsigned char)t[i]);
            digits = digits && isdigit((unsigned char)t[i]);
        }
        int dots = 0;
        for (size_t i = 0; i < colon; i++) {
            if (t[i] == '.')
                dots++;
            else if (!isdigit((unsigned char)t[i]))
                ip = false;
            if (!isalnum((unsigned char)t[i]) && t[i] != '.' && t[i] != '-' && t[i] != '_')
                address = false;
        }
        bool local = colon == 9 && !strncasecmp(t, "localhost", 9);
        if (!(local || (dotted && alpha) || (ip && dots == 3 && digits) || (!dotted && (port || t[host] == '/'))))
            address = false;
    }
    if (address) {
        snprintf(buf, sizeof(buf), "http://%s", t);
    } else {
        int len = snprintf(buf, sizeof(buf), "%s", search);
        len = url_form_encode(buf, len, (int)sizeof(buf), t);
        if (len < 0)
            return NULL;
        buf[len] = 0;
    }
    char *abs = url_resolve(NULL, buf);
    return abs ? abs : strdup(buf);
}

/*
 * A link the browser has nowhere to send: mail, telephone, an app's own
 * scheme. NULL for an address it opens; otherwise a few words to show the
 * user while the page they were reading stays up. Following one used to
 * replace the page with "This kind of address is not supported".
 */
const char *url_unopenable(const char *url)
{
    static const char *const opens[] = { "http", "https", "res", "file", "about", "data" };
    size_t n = 0;

    while (*url && (unsigned char)*url <= ' ')
        url++;
    while (isalnum((unsigned char)url[n]) || (n && (url[n] == '+' || url[n] == '-' || url[n] == '.')))
        n++;
    if (!n || url[n] != ':')
        return NULL;                /* relative: for the loader to judge */
    for (size_t i = 0; i < sizeof(opens) / sizeof(opens[0]); i++)
        if (strlen(opens[i]) == n && !strncasecmp(url, opens[i], n))
            return NULL;
    if (n == 6 && !strncasecmp(url, "mailto", 6))
        return "Can't open mail links";
    if ((n == 3 && (!strncasecmp(url, "tel", 3) || !strncasecmp(url, "sms", 3))) ||
        (n == 6 && !strncasecmp(url, "callto", 6)))
        return "Can't open phone links";
    if (n == 10 && !strncasecmp(url, "javascript", 10))
        return "This link only runs a script";
    return "Can't open this kind of link";
}
