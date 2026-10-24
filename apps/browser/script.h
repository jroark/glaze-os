#ifndef BROWSER_SCRIPT_H
#define BROWSER_SCRIPT_H
#include "web.h"
bool script_init(struct page *p);
void script_free(struct page *p);
bool script_eval(struct page *p, const char *code, int len, struct node *current);
bool script_event(struct page *p, struct node *target, const char *type);
void script_tick(struct page *p);
bool script_allowed(struct page *p, const char *source, const struct node *node);
void script_shutdown(void);
#endif
