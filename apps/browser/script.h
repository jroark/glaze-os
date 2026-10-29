#ifndef BROWSER_SCRIPT_H
#define BROWSER_SCRIPT_H
#include "web.h"
bool script_init(struct page *p);
void script_free(struct page *p);
bool script_eval(struct page *p, const char *code, int len, struct node *current);
bool script_event(struct page *p, struct node *target, const char *type);
void script_tick(struct page *p);
bool script_allowed(struct page *p, const char *source, const struct node *node);
struct fetch_result;
bool script_response(struct page *p, struct fetch_result *r);
/* 0 while dependencies load, 1 on success, -1 on failure. */
int script_module_fetch(struct page *p,const char *url,const struct node *policy);
bool script_module_mime(const char *type);
int script_module(struct page *p, const char *source, int len, struct node *current, const char *url);
void script_shutdown(void);
#endif
