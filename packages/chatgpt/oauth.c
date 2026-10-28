#include "codex.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int error(char *out, size_t cap, const char *message)
{
    snprintf(out, cap, "%s", message); return -1;
}
static bool field(const struct cj *j, const char *key, char *out, size_t cap)
{
    int n = cj_string(j, cj_find(j, 0, key), out, cap);
    if (n <= 0) return false;
    for (int i = 0; i < n; i++) if ((unsigned char)out[i] < 33 || (unsigned char)out[i] > 126) return false;
    return true;
}
static int quote(char *out, size_t cap, const char *text)
{
    size_t n = 0;
    if (cap < 3) return -1;
    out[n++] = '"';
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        if (c < 33 || c > 126 || n + 3 >= cap) return -1;
        if (c == '"' || c == '\\') out[n++] = '\\';
        out[n++] = (char)c;
    }
    out[n++] = '"'; out[n] = 0; return (int)n;
}
static bool form(char *out, size_t cap, size_t *n, const char *key, const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t k = strlen(key);
    if (*n + k + 3 >= cap) return false;
    if (*n) out[(*n)++] = '&';
    memcpy(out + *n, key, k); *n += k; out[(*n)++] = '=';
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*n + 4 >= cap) return false;
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._~", *p))
            out[(*n)++] = (char)*p;
        else { out[(*n)++] = '%'; out[(*n)++] = hex[*p >> 4]; out[(*n)++] = hex[*p & 15]; }
    }
    out[*n] = 0; return true;
}
static void response_free(struct cx_response *r)
{
    if (r->body) { cx_wipe(r->body, r->len); free(r->body); r->body = NULL; }
}
int cx_device_start(struct cx_io *io, struct cx_device *d, char *out, size_t cap)
{
    struct cx_response r = {0};
    int rv = io->post(io, CX_DEVICE, "application/json", "{\"client_id\":\"" CX_CLIENT "\"}", NULL, NULL, NULL, NULL, &r);
    if (rv || !io->active(io->arg)) { response_free(&r); return error(out, cap, "Could not contact OpenAI. Check the network and clock."); }
    if (r.status != 200) { response_free(&r); return error(out, cap, "Device login unavailable. Enable it in your account, or import a Codex login."); }
    struct cj *j = malloc(sizeof(*j));
    bool ok = j && cj_parse(j, r.body, r.len) && field(j, "device_auth_id", d->id, sizeof(d->id));
    uint32_t interval = 5;
    if (ok) {
        int a = cj_find(j, 0, "user_code"), b = cj_find(j, 0, "usercode");
        ok = (a == -1 || b == -1) && (a >= 0 ? field(j, "user_code", d->code, sizeof(d->code)) :
                                                              field(j, "usercode", d->code, sizeof(d->code)));
        int n = cj_find(j, 0, "interval");
        if (n != -1) ok = ok && cj_u32(j, n, &interval) && interval <= 60;
    }
    if (ok) { d->interval_ms = (interval ? interval : 1) * 1000; d->started = io->millis(io->arg); }
    free(j); response_free(&r);
    return ok ? 0 : error(out, cap, "OpenAI returned an invalid device login response.");
}
int cx_device_login(struct cx_io *io, struct cx_tokens *tokens, char *out, size_t cap)
{
    struct cx_device *d = calloc(1, sizeof(*d));
    struct cj *j = malloc(sizeof(*j));
    char *body = malloc(CX_AUTH_MAX), *a = malloc(CX_AUTH_MAX), *b = malloc(CX_AUTH_MAX);
    if (!d || !j || !body || !a || !b) { free(d); free(j); free(body); free(a); free(b); return error(out, cap, "Not enough memory to sign in."); }
    int result = cx_device_start(io, d, out, cap);
    if (result) goto done;
    if (io->progress && io->active(io->arg)) io->progress(io->arg, d->code, true);
    for (;;) {
        if (io->millis(io->arg) - d->started >= 900000) { result = error(out, cap, "Login code expired. Start Sign in again."); break; }
        if (!io->sleep(io->arg, d->interval_ms)) { result = -2; break; }
        if (quote(a, CX_AUTH_MAX, d->id) < 0 || quote(b, CX_AUTH_MAX, d->code) < 0) { result = -1; break; }
        snprintf(body, CX_AUTH_MAX, "{\"device_auth_id\":%s,\"user_code\":%s}", a, b);
        struct cx_response r = {0};
        int rv = io->post(io, CX_POLL, "application/json", body, NULL, NULL, NULL, NULL, &r);
        if (!io->active(io->arg)) { response_free(&r); result = -2; break; }
        bool parsed = !rv && cj_parse(j, r.body, r.len);
        char code[128] = {0};
        if (parsed) field(j, "error", code, sizeof(code));
        bool denied = !strcmp(code, "access_denied") || !strcmp(code, "expired_token");
        if (!rv && !denied && (r.status == 403 || r.status == 404 ||
            (r.status == 400 && !strcmp(code, "authorization_pending")))) { response_free(&r); continue; }
        if (!rv && (r.status == 429 || (r.status == 400 && !strcmp(code, "slow_down")))) {
            if (d->interval_ms < 55000) d->interval_ms += 5000;
            response_free(&r); continue;
        }
        if (rv || r.status != 200 || !parsed) {
            response_free(&r); result = error(out, cap, denied ? "Login was declined or expired. Start again when ready." :
                                                                        "Device login failed. Start Sign in again."); break;
        }
        char verifier[129];
        bool valid = field(j, "authorization_code", a, CX_AUTH_MAX) && field(j, "code_verifier", verifier, sizeof(verifier));
        size_t vlen = valid ? strlen(verifier) : 0;
        valid = valid && vlen >= 43 && vlen <= 128;
        for (size_t i = 0; valid && i < vlen; i++)
            valid = (verifier[i] >= 'a' && verifier[i] <= 'z') || (verifier[i] >= 'A' && verifier[i] <= 'Z') ||
                    (verifier[i] >= '0' && verifier[i] <= '9') || strchr("-._~", verifier[i]);
        response_free(&r);
        size_t n = 0;
        valid = valid && form(body, CX_AUTH_MAX, &n, "grant_type", "authorization_code") &&
                form(body, CX_AUTH_MAX, &n, "client_id", CX_CLIENT) && form(body, CX_AUTH_MAX, &n, "code", a) &&
                form(body, CX_AUTH_MAX, &n, "redirect_uri", CX_REDIRECT) && form(body, CX_AUTH_MAX, &n, "code_verifier", verifier);
        cx_wipe(verifier, sizeof(verifier));
        if (!valid) { result = error(out, cap, "Invalid authorization code response."); break; }
        /* One exchange only: a transport timeout may have consumed the code. */
        rv = io->post(io, CX_TOKEN, "application/x-www-form-urlencoded", body, NULL, NULL, NULL, NULL, &r);
        cx_wipe(body, CX_AUTH_MAX);
        if (!io->active(io->arg)) { response_free(&r); result = -2; break; }
        valid = !rv && r.status == 200 && cx_grant(r.body, r.len, NULL, io->time(io->arg), tokens);
        response_free(&r);
        if (!valid) { result = error(out, cap, "Token exchange failed. Start a new Sign in attempt."); break; }
        result = io->save(io->arg, tokens) ? 0 : error(out, cap, "Signed in, but could not save the login. Check storage.");
        break;
    }
done:
    cx_wipe(d, sizeof(*d)); cx_wipe(body, CX_AUTH_MAX); cx_wipe(a, CX_AUTH_MAX); cx_wipe(b, CX_AUTH_MAX);
    free(d); free(j); free(body); free(a); free(b); return result;
}
int cx_refresh(struct cx_io *io, struct cx_tokens *t, char *out, size_t cap)
{
    char *body = malloc(CX_AUTH_MAX), *quoted = malloc(CX_AUTH_MAX);
    struct cx_tokens *next = malloc(sizeof(*next));
    if (!body || !quoted || !next) { free(body); free(quoted); free(next); return error(out, cap, "Not enough memory to refresh sign-in."); }
    int result = -1;
    if (quote(quoted, CX_AUTH_MAX, t->refresh) < 0) goto done;
    snprintf(body, CX_AUTH_MAX, "{\"grant_type\":\"refresh_token\",\"client_id\":\"" CX_CLIENT "\",\"refresh_token\":%s}", quoted);
    struct cx_response r = {0};
    int rv = io->post(io, CX_TOKEN, "application/json", body, NULL, NULL, NULL, NULL, &r);
    if (!io->active(io->arg)) result = -2;
    else if (!rv && r.status == 200 && cx_grant(r.body, r.len, t, io->time(io->arg), next)) {
        *t = *next; /* Keep rotated credentials even if storage is temporarily full. */
        result = io->save(io->arg, t) ? 0 : error(out, cap, "Could not save refreshed sign-in. Check storage and Retry.");
    } else if (!rv && (r.status == 400 || r.status == 401)) {
        cx_wipe(t, sizeof(*t)); io->save(io->arg, t);
        result = error(out, cap, "Sign-in expired or was revoked. Sign in again.");
    } else result = error(out, cap, "Sign-in refresh failed. Check the network, then Retry.");
    response_free(&r);
done:
    cx_wipe(body, CX_AUTH_MAX); cx_wipe(quoted, CX_AUTH_MAX); cx_wipe(next, sizeof(*next));
    free(body); free(quoted); free(next); return result;
}
