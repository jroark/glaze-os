/* One worker owns HTTP, OAuth and token persistence. UI jobs carry immutable
 * snapshots; generation checks discard results from canceled/replaced jobs. */
#include "codex.h"
#include "plat.h"
#include <glaze/fs.h>
#include <glaze/res.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct gz_mutex lock = GZ_MUTEX_INIT;
static int wake;
static unsigned serial;
static bool ready;
static struct chat_result completion;
static struct command { struct chat_config config; char *body; int kind; } pending;
struct context { unsigned gen; struct chat_config config; struct cx_tokens *tokens; };
static void path(char *out, const char *name) { snprintf(out, FS_PATH_MAX, "%s/data/%s", res_bundle(), name); }
static bool active(void *arg)
{
    struct context *c = arg;
    gz_mutex_lock(&lock); bool yes = serial == c->gen; gz_mutex_unlock(&lock); return yes;
}
static uint32_t millis(void *arg) { (void)arg; return gz_time_ms(); }
static uint32_t unix_time(void *arg) { (void)arg; return (uint32_t)plat_time(); }
static bool pause(void *arg, uint32_t ms)
{
    uint32_t start = gz_time_ms();
    while (active(arg)) { if (gz_time_ms() - start >= ms) return true; gz_sleep_ms(10); }
    return false;
}
static bool erase(void)
{
    char file[FS_PATH_MAX]; path(file, "auth.tmp"); int temp = fs_remove(file); path(file, "auth");
    int rv = fs_remove(file);
    return (temp == 0 || temp == GZ_ENOENT) && (rv == 0 || rv == GZ_ENOENT);
}
static bool save(void *arg, const struct cx_tokens *tokens)
{
    struct context *c = arg;
    if (!active(c)) return false;
    if (!c->config.remember || !cx_tokens_valid(tokens)) return erase();
    char file[FS_PATH_MAX], temp[FS_PATH_MAX]; path(file, "auth"); path(temp, "auth.tmp");
    if (fs_write_file(temp, tokens, sizeof(*tokens)) != (int)sizeof(*tokens)) { fs_remove(temp); return false; }
    if (!active(c)) { fs_remove(temp); return false; }
    if (fs_rename(temp, file)) { fs_remove(temp); return false; }
    return true;
}
static void publish(void *arg, const char *text, bool device)
{
    struct context *c = arg;
    gz_mutex_lock(&lock);
    if (c->gen == serial) {
        memset(&completion, 0, sizeof(completion));
        completion.kind = device ? CHAT_DEVICE : CHAT_PARTIAL;
        completion.signed_in = cx_tokens_valid(c->tokens);
        snprintf(completion.text, sizeof(completion.text), "%s", text);
        ready = true;
    }
    gz_mutex_unlock(&lock);
}
static bool import(struct context *c, char *message, size_t cap)
{
    const char *paths[] = {"/Documents/CODEX.AUTH", "/Downloads/CODEX.AUTH", "/Card/CODEX.AUTH"};
    char *data = malloc(CX_AUTH_MAX + 1);
    struct cx_tokens *tokens = calloc(1, sizeof(*tokens));
    if (!data || !tokens) { free(data); free(tokens); return false; }
    bool ok = false;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct fs_stat st;
        if (fs_stat(paths[i], &st) || !st.size || st.size > CX_AUTH_MAX) continue;
        if (fs_read_file(paths[i], data, st.size) != (int)st.size || !cx_import(data, st.size, tokens)) continue;
        if (!active(c)) break;
        *c->tokens = *tokens;
        ok = save(c, c->tokens);
        snprintf(message, cap, ok ? "Codex login imported." : "Login imported, but could not save it. Check storage.");
        break;
    }
    if (!cx_tokens_valid(tokens)) snprintf(message, cap, "Copy CODEX.AUTH to Documents or the card, then Import.");
    cx_wipe(data, CX_AUTH_MAX + 1); cx_wipe(tokens, sizeof(*tokens)); free(data); free(tokens); return ok;
}
static void run(void *unused)
{
    struct cx_tokens *tokens = unused;
    bool loaded = false;
    for (;;) {
        gz_sem_wait(wake, -1);
        gz_mutex_lock(&lock);
        struct command cmd = pending; pending.body = NULL; pending.kind = -1;
        struct context c = {.gen = serial, .config = cmd.config, .tokens = tokens};
        gz_mutex_unlock(&lock);
        if (cmd.kind < 0) continue;
        if (!loaded) {
            char file[FS_PATH_MAX]; path(file, "auth");
            if (!cmd.config.remember || fs_read_file(file, tokens, sizeof(*tokens)) != (int)sizeof(*tokens) || !cx_tokens_valid(tokens))
                cx_wipe(tokens, sizeof(*tokens));
            if (!cmd.config.remember) erase();
            loaded = true;
        }
        struct cx_io io = {.arg = &c, .active = active, .sleep = pause, .millis = millis, .time = unix_time,
                           .save = save, .progress = publish, .post = cx_http_post};
        char *text = calloc(1, CHAT_TEXT);
        bool ok = false;
        if (text) {
            if (cmd.kind == CHAT_BOOT) ok = true;
            else if (cmd.kind == CHAT_LOGIN) ok = cx_device_login(&io, tokens, text, CHAT_TEXT) == 0;
            else if (cmd.kind == CHAT_IMPORT) ok = import(&c, text, CHAT_TEXT);
            else if (cmd.kind == CHAT_LOGOUT) { ok = erase(); if (ok) cx_wipe(tokens, sizeof(*tokens)); snprintf(text, CHAT_TEXT, ok ? "Signed out." : "Could not remove saved login. Check storage and sign out again."); }
            else if (cmd.kind == CHAT_SEND && cmd.body) ok = cx_chat(&io, tokens, cmd.body, text, CHAT_TEXT) == 0;
        }
        if (!active(&c) && (cmd.kind == CHAT_LOGIN || cmd.kind == CHAT_IMPORT)) {
            cx_wipe(tokens, sizeof(*tokens)); erase(); /* A canceled login must not reappear after restart. */
        }
        if (cmd.body) { cx_wipe(cmd.body, strlen(cmd.body)); free(cmd.body); }
        gz_mutex_lock(&lock);
        if (c.gen == serial) {
            memset(&completion, 0, sizeof(completion)); completion.kind = cmd.kind;
            completion.success = ok; completion.signed_in = cx_tokens_valid(tokens);
            snprintf(completion.text, sizeof(completion.text), "%s", text ? text : "Not enough memory.");
            snprintf(completion.plan, sizeof(completion.plan), "%s", tokens->plan);
            ready = true;
        }
        gz_mutex_unlock(&lock); free(text);
    }
}
static bool submit(int kind, const struct chat_config *config, char *body)
{
    if (!wake) return false;
    gz_mutex_lock(&lock); serial++; ready = false;
    if (pending.body) { cx_wipe(pending.body, strlen(pending.body)); free(pending.body); }
    pending.kind = kind; pending.config = *config; pending.body = body;
    gz_mutex_unlock(&lock); gz_sem_post(wake); return true;
}
bool chat_worker_init(const struct chat_config *config)
{
    struct cx_tokens *tokens = calloc(1, sizeof(*tokens));
    if (!tokens) return false;
    wake = gz_sem_create(0);
    if (wake < 0) { wake = 0; free(tokens); return false; }
    pending.kind = -1;
    if (gz_thread_create(run, tokens) < 0) { gz_sem_destroy(wake); wake = 0; free(tokens); return false; }
    return submit(CHAT_BOOT, config, NULL);
}
bool chat_worker_send(const struct chat_config *config, char *body, int len)
{
    return body && len > 0 && submit(CHAT_SEND, config, body);
}
bool chat_worker_command(int kind, const struct chat_config *config)
{
    return (kind == CHAT_LOGIN || kind == CHAT_IMPORT || kind == CHAT_LOGOUT) && submit(kind, config, NULL);
}
void chat_worker_cancel(void)
{
    gz_mutex_lock(&lock); serial++; ready = false;
    if (pending.body) { cx_wipe(pending.body, strlen(pending.body)); free(pending.body); pending.body = NULL; }
    pending.kind = -1; gz_mutex_unlock(&lock);
}
bool chat_worker_poll(struct chat_result *out)
{
    gz_mutex_lock(&lock); bool have = ready;
    if (have) { *out = completion; ready = false; }
    gz_mutex_unlock(&lock); return have;
}
