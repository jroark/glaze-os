/* DOM edits share the page arena, preserving native node identity until unload. */
#include "web.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MUTATION_ARENA_MAX (4u * 1024u * 1024u)
static bool room(struct page *p, size_t bytes)
{
    return bytes < 65537 && p->arena.used + bytes < MUTATION_ARENA_MAX &&
           p->dom_nodes < HTML_MAX_NODES;
}
static void changed(struct page *p) { p->dirty = p->resources_dirty = true; }
struct node *dom_create(struct page *p, int type, const char *name, const char *text)
{
    if (!p || !room(p, text ? strlen(text) : 0))
        return NULL;
    struct node *n = arena_alloc(&p->arena, sizeof *n);
    if (!n)
        return NULL;
    n->owner = p;
    n->type = (uint8_t)type;
    n->img = n->frame = n->nsdecl = -1;
    p->dom_nodes++;
    if (type == N_ELEM) {
        if (!name || !*name || strlen(name) > 63)
            return NULL;
        char *lower = arena_strndup(&p->arena, name, (int)strlen(name));
        if (!lower)
            return NULL;
        for (char *c = lower; *c; c++)
            *c = (char)tolower((unsigned char)*c);
        n->name = lower;
        n->tag = tag_lookup(lower, (int)strlen(lower));
    } else if (type == N_TEXT) {
        n->len = text ? (int)strlen(text) : 0;
        n->text = arena_strndup(&p->arena, text ? text : "", n->len);
        if (!n->text)
            return NULL;
    }
    return n;
}
void dom_remove(struct page *p, struct node *n)
{
    if (!n || !n->parent || n == p->html || n == p->body || n == p->doc)
        return;
    if (n->prev)
        n->prev->next = n->next;
    else
        n->parent->first = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        n->parent->last = n->prev;
    n->parent = n->prev = n->next = NULL;
    changed(p);
}
bool dom_insert(struct page *p, struct node *parent, struct node *child, struct node *before)
{
    if (!parent || !child || parent->owner != p || child->owner != p || parent->type == N_TEXT ||
        child == p->html || child == p->body || child == p->doc ||
        (before && before->parent != parent))
        return false;
    int depth = 0;
    for (struct node *a = parent; a; a = a->parent) {
        if (a == child || ++depth >= HTML_MAX_DEPTH)
            return false;
    }
    for (struct node *n = child; n; n = node_next(n, child, false)) {
        int d = depth;
        for (struct node *a = n; a && a != child; a = a->parent)
            if (++d >= HTML_MAX_DEPTH)
                return false;
    }
    if (child == before)
        return true;
    dom_remove(p, child);
    child->parent = parent;
    child->next = before;
    child->prev = before ? before->prev : parent->last;
    if (child->prev)
        child->prev->next = child;
    else
        parent->first = child;
    if (before)
        before->prev = child;
    else
        parent->last = child;
    changed(p);
    return true;
}
bool dom_attr(struct page *p, struct node *n, const char *name, const char *value)
{
    if (!n || n->owner != p || n->type != N_ELEM || !name || strlen(name) > 127 ||
        !room(p, value ? strlen(value) : 0))
        return false;
    char lower[128];
    snprintf(lower, sizeof lower, "%s", name);
    for (char *s = lower; *s; s++)
        *s = (char)tolower((unsigned char)*s);
    struct attr **a = &n->attrs;
    while (*a && strcmp((*a)->name, lower))
        a = &(*a)->next;
    if (!value) {
        if (*a)
            *a = (*a)->next;
    } else {
        if (!*a) {
            *a = arena_alloc(&p->arena, sizeof **a);
            if (!*a)
                return false;
            (*a)->name = arena_strndup(&p->arena, lower, (int)strlen(lower));
        }
        (*a)->value = arena_strndup(&p->arena, value, (int)strlen(value));
        if (!(*a)->value)
            return false;
    }
    n->nsdecl = -1;
    if (!strcmp(lower, "src") || !strcmp(lower, "href") || !strcmp(lower, "srcdoc") ||
        !strcmp(lower, "sandbox"))
        n->collected = 0;
    if (n->ctl) {
        if (!strcmp(lower, "disabled"))
            n->ctl->disabled = value != NULL;
        if (!strcmp(lower, "checked"))
            n->ctl->checked = value != NULL;
        if (!strcmp(lower, "value")) {
            snprintf(n->ctl->value, (size_t)n->ctl->cap, "%s", value ? value : "");
            n->ctl->len = (int)strlen(n->ctl->value);
        }
    }
    changed(p);
    return true;
}
static void assign_owner(struct page *p, struct node *root)
{
    for (struct node *n = root; n; n = node_next(n, root, false)) {
        n->owner = p;
        p->dom_nodes++;
    }
}
bool dom_content(struct page *p, struct node *n, const char *text, bool html)
{
    if (!n || n->owner != p || !room(p, text ? strlen(text) : 0))
        return false;
    if (n->tag == T_STYLE)
        n->collected = 0;
    if (n->parent && n->parent->tag == T_STYLE)
        n->parent->collected = 0;
    if (n->type == N_TEXT) {
        n->len = text ? (int)strlen(text) : 0;
        n->text = arena_strndup(&p->arena, text ? text : "", n->len);
        n->ctext = NULL;
        n->ckey = 0;
        changed(p);
        return n->text != NULL;
    }
    while (n->first) {
        struct node *c = n->first;
        /* body/html cannot be detached by removeChild, but replacing children
         * still severs every former child, preserving detached node wrappers. */
        n->first = c->next;
        c->parent = c->prev = c->next = NULL;
    }
    n->last = NULL;
    if (html && text && *text) {
        struct node *doc = html_parse(&p->arena, text, (int)strlen(text));
        if (!doc)
            return false;
        struct node *h = node_first_elem(doc), *head = NULL, *body = NULL;
        for (struct node *c = h ? h->first : NULL; c; c = c->next) {
            if (c->tag == T_HEAD)
                head = c;
            if (c->tag == T_BODY)
                body = c;
        }
        /* Fragment script/style nodes can land in the parser's implicit head. */
        for (int part = 0; part < 2; part++) {
            struct node *from = part == 0 ? head : body;
            while (from && from->first) {
                struct node *c = from->first;
                from->first = c->next;
                c->parent = c->prev = c->next = NULL;
                assign_owner(p, c);
                if (!dom_insert(p, n, c, NULL))
                    return false;
            }
        }
    } else if (text && *text) {
        struct node *t = dom_create(p, N_TEXT, NULL, text);
        if (!t || !dom_insert(p, n, t, NULL))
            return false;
    }
    changed(p);
    return true;
}
struct out {
    char *data;
    size_t len, cap;
    bool failed;
};
static void put(struct out *o, const char *s, size_t n)
{
    if (o->failed)
        return;
    if (o->len + n > 65536) {
        o->failed = true;
        return;
    }
    if (o->len + n + 1 > o->cap) {
        size_t cap = (o->len + n + 1) * 2;
        char *p = realloc(o->data, cap);
        if (!p) {
            o->failed = true;
            return;
        }
        o->data = p;
        o->cap = cap;
    }
    memcpy(o->data + o->len, s, n);
    o->len += n;
    o->data[o->len] = 0;
}
static void escaped(struct out *o, const char *s, bool attr)
{
    for (; s && *s; s++) {
        const char *v = *s == '&'           ? "&amp;"
                        : *s == '<'         ? "&lt;"
                        : attr && *s == '"' ? "&quot;"
                                            : NULL;
        if (v)
            put(o, v, strlen(v));
        else
            put(o, s, 1);
    }
}
static void serialize(struct out *o, struct node *n, int depth)
{
    if (depth > HTML_MAX_DEPTH)
        return;
    if (n->type == N_TEXT) {
        escaped(o, n->text, false);
        return;
    }
    if (n->type == N_ELEM) {
        put(o, "<", 1);
        put(o, n->name, strlen(n->name));
        for (struct attr *a = n->attrs; a; a = a->next) {
            put(o, " ", 1);
            put(o, a->name, strlen(a->name));
            put(o, "=\"", 2);
            escaped(o, a->value, true);
            put(o, "\"", 1);
        }
        put(o, ">", 1);
    }
    for (struct node *c = n->first; c; c = c->next)
        serialize(o, c, depth + 1);
    if (n->type == N_ELEM && n->tag != T_INPUT && n->tag != T_IMG && n->tag != T_BR &&
        n->tag != T_HR && n->tag != T_FRAME && n->tag != T_META && n->tag != T_LINK) {
        put(o, "</", 2);
        put(o, n->name, strlen(n->name));
        put(o, ">", 1);
    }
}
char *dom_serialize(struct page *p, struct node *n, bool outer)
{
    (void)p;
    struct out o = {0};
    put(&o, "", 0);
    if (outer)
        serialize(&o, n, 0);
    else
        for (struct node *c = n->first; c; c = c->next)
            serialize(&o, c, 0);
    if (o.failed) {
        free(o.data);
        return NULL;
    }
    return o.data;
}
int dom_select(struct page *p, struct node *root, const char *selector, struct node **out, int max)
{
    (void)p;
    if (!root || !selector || strlen(selector) > 512)
        return 0;
    char text[540];
    snprintf(text, sizeof text, "%s{color:black}", selector);
    struct arena a = {0};
    struct stylesheet ss = {0};
    css_parse_sheet(&a, &ss, text, (int)strlen(text));
    int count = 0;
    for (struct node *n = root->first; n && count < max; n = node_next(n, root, false)) {
        bool match = false;
        for (int i = 0; i < ss.nrules && !match; i++)
            for (int j = 0; j < ss.rules[i].nsels; j++)
                if (css_node_matches(&ss.rules[i].sels[j], n)) {
                    match = true;
                    break;
                }
        if (match)
            out[count++] = n;
    }
    css_free_sheet(&ss);
    arena_free(&a);
    return count;
}
struct node *dom_query(struct page *p, struct node *root, const char *selector, struct node *after)
{
    struct node *nodes[256];
    int n = dom_select(p, root, selector, nodes, 256);
    if (!after)
        return n ? nodes[0] : NULL;
    for (int i = 0; i + 1 < n; i++)
        if (nodes[i] == after)
            return nodes[i + 1];
    return NULL;
}
