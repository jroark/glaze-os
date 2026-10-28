/* Codex-compatible protocol facts: openai/codex @
 * 84bef6ea8c747f01216df126bbb40fbc12a8adb1 (2026-10-28).
 * This client has no tools, shell execution or web-session cookie access. */
#ifndef GLAZE_CODEX_CHAT_H
#define GLAZE_CODEX_CHAT_H
#include "chat.h"
#define CX_CLIENT "app_EMoamEEZ73f0CkXaXp7hrann"
#define CX_ISSUER "https://auth.openai.com"
#define CX_DEVICE CX_ISSUER "/api/accounts/deviceauth/usercode"
#define CX_POLL CX_ISSUER "/api/accounts/deviceauth/token"
#define CX_TOKEN CX_ISSUER "/oauth/token"
#define CX_VERIFY CX_ISSUER "/codex/device"
#define CX_REDIRECT CX_ISSUER "/deviceauth/callback"
#define CX_RESPONSES "https://chatgpt.com/backend-api/codex/responses"
#define CX_ACCESS 8192
#define CX_REFRESH 4096
#define CX_AUTH_MAX 32768
#define CX_EVENT_MAX 65536
#define CX_STREAM_MAX (512 * 1024)
struct cx_tokens {
    uint32_t version, expires;
    char access[CX_ACCESS], refresh[CX_REFRESH], account[129], plan[40];
};
struct cx_device { char id[256], code[40]; uint32_t interval_ms, started; };
struct cx_response { int status; char *body; size_t len; char type[64]; };
struct cx_io {
    void *arg;
    bool (*active)(void *);
    bool (*sleep)(void *, uint32_t);
    uint32_t (*millis)(void *), (*time)(void *);
    bool (*save)(void *, const struct cx_tokens *);
    void (*progress)(void *, const char *, bool device);
    int (*post)(struct cx_io *, const char *url, const char *type, const char *body,
                const char *bearer, const char *account,
                int (*sink)(const uint8_t *, int, void *), void *sink_arg, struct cx_response *);
};
void cx_wipe(void *, size_t);
bool cx_tokens_valid(const struct cx_tokens *);
bool cx_import(const void *json, size_t len, struct cx_tokens *out);
bool cx_grant(const void *json, size_t len, const struct cx_tokens *old, uint32_t now, struct cx_tokens *out);
int cx_device_start(struct cx_io *, struct cx_device *, char *error, size_t cap);
int cx_device_login(struct cx_io *, struct cx_tokens *, char *error, size_t cap);
int cx_refresh(struct cx_io *, struct cx_tokens *, char *error, size_t cap);
int cx_chat(struct cx_io *, struct cx_tokens *, const char *body, char *text, size_t cap);
int cx_http_post(struct cx_io *, const char *, const char *, const char *, const char *, const char *,
                 int (*)(const uint8_t *, int, void *), void *, struct cx_response *);
bool chat_model_ok(const char *);
int chat_codex_request(const struct chat_history *, const char *model, char *out, size_t cap);
#endif
