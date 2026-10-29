#include "codex.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
void cx_wipe(void *p, size_t n) { volatile unsigned char *v = p; while (n--) *v++ = 0; }
static bool ascii(const char *s, size_t cap)
{
    size_t n = strnlen(s, cap);
    if (!n || n == cap) return false;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 33 || (unsigned char)s[i] > 126) return false;
    return true;
}
bool cx_tokens_valid(const struct cx_tokens *t)
{
    return t->version == 1 && t->expires && ascii(t->access, sizeof(t->access)) &&
           ascii(t->refresh, sizeof(t->refresh)) && ascii(t->account, sizeof(t->account)) &&
           memchr(t->plan, 0, sizeof(t->plan));
}
static int b64(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}
/* JWT claims are metadata, not a local proof of identity. Only the fixed
 * OpenAI endpoints validate the bearer; claims never choose an authority. */
static bool claims(struct cj *j, const char *jwt, char *decoded, size_t cap)
{
    const char *p = strchr(jwt, '.'); if (!p || p == jwt) return false;
    const char *end = strchr(++p, '.');
    if (!end || end == p || !end[1] || strchr(end + 1, '.')) return false;
    uint32_t bits = 0; unsigned nbits = 0; size_t n = 0;
    while (p < end) {
        int v = b64((unsigned char)*p++); if (v < 0) return false;
        bits = bits << 6 | (unsigned)v; nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (n == cap) return false;
            decoded[n++] = (char)(bits >> nbits);
        }
    }
    if (nbits >= 6 || (nbits && (bits & ((1u << nbits) - 1)))) return false;
    return cj_parse(j, decoded, n) && j->nodes[0].kind == CJ_OBJECT;
}
static bool read_string(const struct cj *j, int obj, const char *key, char *out, size_t cap, bool optional)
{
    int id = cj_find(j, obj, key);
    if (optional && (id == -1 || cj_is(j, id, "null"))) return true;
    memset(out, 0, cap); /* Shorter rotations must not retain an old token tail. */
    return cj_string(j, id, out, cap) > 0 && ascii(out, cap);
}
static bool metadata(struct cx_tokens *t, const char *id_token)
{
    struct cj *j = malloc(sizeof(*j)); char *decoded = malloc(CX_ACCESS);
    if (!j || !decoded) { free(j); free(decoded); return false; }
    bool valid = claims(j, t->access, decoded, CX_ACCESS) &&
                 cj_u32(j, cj_find(j, 0, "exp"), &t->expires);
    int auth = valid ? cj_find(j, 0, "https://api.openai.com/auth") : -1;
    char account[129] = {0};
    if (auth >= 0) {
        valid = read_string(j, auth, "chatgpt_account_id", account, sizeof(account), false);
        if (cj_is(j, cj_find(j, auth, "chatgpt_account_is_fedramp"), "true")) valid = false;
        if (cj_find(j, auth, "chatgpt_plan_type") >= 0)
            valid = valid && read_string(j, auth, "chatgpt_plan_type", t->plan, sizeof(t->plan), false);
    } else valid = false;
    if (valid && t->account[0] && strcmp(account, t->account)) valid = false;
    if (valid) memcpy(t->account, account, sizeof(account));
    /* Some grants put plan metadata only in the ID token. Access-token
     * account selection remains authoritative for routing the bearer. */
    if (valid && id_token && *id_token && claims(j, id_token, decoded, CX_ACCESS)) {
        auth = cj_find(j, 0, "https://api.openai.com/auth");
        if (auth >= 0) {
            if (cj_is(j, cj_find(j, auth, "chatgpt_account_is_fedramp"), "true")) valid = false;
            if (!t->plan[0]) read_string(j, auth, "chatgpt_plan_type", t->plan, sizeof(t->plan), true);
        }
    }
    cx_wipe(decoded, CX_ACCESS); free(decoded); free(j); return valid;
}
bool cx_import(const void *data, size_t len, struct cx_tokens *out)
{
    if (len > CX_AUTH_MAX) return false;
    struct cj *j = malloc(sizeof(*j)); char *id = calloc(1, CX_ACCESS);
    struct cx_tokens *t = calloc(1, sizeof(*t));
    if (!j || !id || !t) { free(j); free(id); free(t); return false; }
    bool ok = cj_parse(j, data, len);
    int obj = ok ? cj_find(j, 0, "tokens") : -1;
    char mode[64]; int m = ok ? cj_find(j, 0, "auth_mode") : -2;
    if (m >= 0) ok = cj_string(j, m, mode, sizeof(mode)) > 0 && !strcmp(mode, "chatgpt");
    else if (m == -2) ok = false;
    int key = ok ? cj_find(j, 0, "OPENAI_API_KEY") : -2;
    if (key == -2 || (key >= 0 && !cj_is(j, key, "null"))) ok = false;
    t->version = 1;
    if (ok && obj >= 0) {
        ok = read_string(j, obj, "access_token", t->access, sizeof(t->access), false) &&
             read_string(j, obj, "refresh_token", t->refresh, sizeof(t->refresh), false) &&
             read_string(j, obj, "account_id", t->account, sizeof(t->account), true) &&
             read_string(j, obj, "id_token", id, CX_ACCESS, true);
        int provenance = cj_find(j, obj, "oauth_provenance");
        if (provenance == -2) ok = false;
        if (provenance >= 0 && !cj_is(j, provenance, "null")) {
            char issuer[128], client[128];
            ok = ok && read_string(j, provenance, "issuer", issuer, sizeof(issuer), false) &&
                 read_string(j, provenance, "client_id", client, sizeof(client), false) &&
                 !strcmp(issuer, CX_ISSUER) && !strcmp(client, CX_CLIENT);
        }
    } else ok = false;
    if (ok) ok = metadata(t, id) && cx_tokens_valid(t);
    if (ok) *out = *t;
    cx_wipe(id, CX_ACCESS); cx_wipe(t, sizeof(*t)); free(t); free(id); free(j); return ok;
}
bool cx_grant(const void *data, size_t len, const struct cx_tokens *old, uint32_t now, struct cx_tokens *out)
{
    (void)now;
    struct cj *j = malloc(sizeof(*j)); char *id = calloc(1, CX_ACCESS);
    struct cx_tokens *t = calloc(1, sizeof(*t));
    if (!j || !id || !t) { free(j); free(id); free(t); return false; }
    if (old) *t = *old;
    t->version = 1;
    bool ok = len <= CX_AUTH_MAX && cj_parse(j, data, len) &&
              read_string(j, 0, "access_token", t->access, sizeof(t->access), false) &&
              read_string(j, 0, "refresh_token", t->refresh, sizeof(t->refresh), old != NULL) &&
              read_string(j, 0, "id_token", id, CX_ACCESS, true);
    if (ok) ok = metadata(t, id) && cx_tokens_valid(t);
    if (ok) *out = *t;
    cx_wipe(id, CX_ACCESS); cx_wipe(t, sizeof(*t)); free(t); free(id); free(j); return ok;
}
