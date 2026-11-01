#include "chat.h"
#include "codex.h"
#include "json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    struct chat_history *h = calloc(1, sizeof(*h));
    char *wire = malloc(CHAT_WIRE), out[CHAT_TEXT];
    assert(h && wire); h->version = 1;
    assert(!chat_append(h, true, "bad role"));
    assert(chat_append(h, false, "Hello \"world\"\n\\caf\xe9"));
    int n = chat_request(h, wire, CHAT_WIRE);
    assert(n > 0 && strstr(wire, "Hello \\\"world\\\"\\n\\\\caf\xc3\xa9"));
    assert(chat_request(h, wire, 5) == -1);
    assert(chat_append(h, true, "reply"));
    assert(chat_request(h, wire, CHAT_WIRE) == -1); /* Needs a user turn. */
    for (int i = 0; i < 20; i++) {
        char text[24]; snprintf(text, sizeof(text), "turn %d", i);
        assert(chat_append(h, false, text)); assert(chat_append(h, true, "reply"));
    }
    assert(h->count == CHAT_COUNT && !strcmp(h->messages[0].text, "turn 14"));
    h->messages[0].assistant = 1; assert(!chat_history_valid(h)); h->messages[0].assistant = 0;
    assert(chat_text((const uint8_t *)"caf\xc3\xa9 \xe2\x80\x94 \xf0\x9f\x98\x80", 14, out, sizeof(out)));
    assert(!strcmp(out, "caf\xe9 - ?"));
    static const uint8_t invalid[][5] = {{0xc0, 0x80}, {0xed, 0xa0, 0x80}, {0xf4, 0x90, 0x80, 0x80}, {0}, {0xe2, 0x82}};
    const size_t lengths[] = {2, 3, 4, 1, 2};
    for (int i = 0; i < 5; i++) assert(!chat_text(invalid[i], lengths[i], out, sizeof(out)));
    uint8_t long_text[CHAT_TEXT * 2]; memset(long_text, 'a', sizeof(long_text));
    assert(chat_text(long_text, sizeof(long_text), out, sizeof(out)) && strstr(out, "[Reply shortened]"));
    memset(long_text, '\n', sizeof(long_text));
    assert(chat_text(long_text, sizeof(long_text), out, sizeof(out)) && strlen(out) < 200);
    assert(chat_model_ok("gpt-6-luna") && !chat_model_ok("gpt-6\r\nInjected: yes"));
    assert(chat_append(h, false, "next"));
    n = chat_codex_request(h, "gpt-6-luna", wire, CHAT_WIRE);
    struct cj *j = malloc(sizeof(*j)); assert(j && n > 0 && cj_parse(j, wire, (size_t)n));
    assert(cj_is(j, cj_find(j, 0, "store"), "false"));
    assert(cj_is(j, cj_find(j, 0, "stream"), "true"));
    assert(cj_find(j, 0, "input") >= 0 && cj_find(j, 0, "tools") >= 0);
    assert(cj_find(j, 0, "max_output_tokens") == -1);
    const char *duplicate = "{\"access_token\":\"one\",\"access_token\":\"two\"}";
    assert(cj_parse(j, duplicate, strlen(duplicate)) && cj_find(j, 0, "access_token") == -2);
    const char *unicode = "{\"a\":\"\\u00e9\\ud83d\\ude00\"}";
    assert(cj_parse(j, unicode, strlen(unicode)) && cj_string(j, cj_find(j, 0, "a"), out, sizeof(out)) == 6);
    assert(!strcmp(out, "\xc3\xa9\xf0\x9f\x98\x80"));
    const char *bad[] = {"{\"a\":1,}", "[1,]", "{\"a\":+1}", "[01]", "[true false]", "[1e]", "[\"bad\\x\"]"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) assert(!cj_parse(j, bad[i], strlen(bad[i])));
    char fuzz[300];
    for (int k = 0; k < 10000; k++) {
        size_t len = (size_t)(rand() % sizeof(fuzz));
        for (size_t i = 0; i < len; i++) fuzz[i] = (char)(rand() & 127);
        if (cj_parse(j, fuzz, len)) { cj_find(j, 0, "anything"); if (j->nodes[0].kind == CJ_STRING) cj_string(j, 0, out, sizeof(out)); }
    }
    free(j);
    free(h); free(wire); puts("chatgpt: protocol ALL PASS");
    return 0;
}
