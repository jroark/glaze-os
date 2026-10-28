#ifndef CHATGPT_CHAT_H
#define CHATGPT_CHAT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHAT_COUNT 12
#define CHAT_TEXT 4096
#define CHAT_DRAFT 1024
#define CHAT_WIRE (128 * 1024)
#define CHAT_RESPONSE (24 * 1024)
struct chat_message { uint32_t assistant; char text[CHAT_TEXT]; };
struct chat_history { uint32_t version, count; struct chat_message messages[CHAT_COUNT]; };
struct chat_config {
    uint32_t version, remember;
    char model[64];
};
bool chat_history_valid(const struct chat_history *h);
bool chat_append(struct chat_history *h, bool assistant, const char *text);
/* Latin-1 app text -> UTF-8 JSON; returns length, or -1 on invalid/full. */
int chat_request(const struct chat_history *h, char *out, size_t cap);
/* Strict UTF-8 -> device glyphs, with a visible truncation suffix if needed. */
bool chat_text(const uint8_t *data, size_t len, char *out, size_t cap);

/* Network worker owns all HTTP/TLS state and an immutable request snapshot. */
enum { CHAT_BOOT, CHAT_SEND, CHAT_LOGIN, CHAT_IMPORT, CHAT_LOGOUT, CHAT_DEVICE, CHAT_PARTIAL };
struct chat_result { int kind; bool success, signed_in; char text[CHAT_TEXT], plan[40]; };
bool chat_worker_init(const struct chat_config *);
bool chat_worker_send(const struct chat_config *config, char *request, int len);
bool chat_worker_command(int kind, const struct chat_config *config);
void chat_worker_cancel(void);
/* One completion; error is a readable message, not a transcript entry. */
bool chat_worker_poll(struct chat_result *);
#endif
