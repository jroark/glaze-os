/* Bounded JSON reader used for OAuth responses and SSE events. */
#ifndef CHAT_JSON_H
#define CHAT_JSON_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CJ_MAX 2048
enum { CJ_OBJECT, CJ_ARRAY, CJ_STRING, CJ_NUMBER, CJ_LITERAL };
struct cj_node { int start, end, next, kind; };
struct cj { const char *data; int len, count; struct cj_node nodes[CJ_MAX]; };
bool cj_parse(struct cj *j, const void *data, size_t len);
/* -1 absent, -2 duplicate/invalid. */
int cj_find(const struct cj *j, int object, const char *key);
int cj_string(const struct cj *j, int node, char *out, size_t cap);
bool cj_u32(const struct cj *j, int node, uint32_t *out);
bool cj_is(const struct cj *j, int node, const char *literal);
#endif
