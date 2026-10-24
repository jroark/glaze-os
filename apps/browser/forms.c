/* Form controls: collection, state and submission. */

#include "url.h"
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FORM_DATA_MAX   8192

static int input_type(const char *t)
{
    static const struct {
        const char *name;
        uint8_t type;
    } types[] = {
        { "password", C_PASSWORD }, { "checkbox", C_CHECKBOX }, { "radio", C_RADIO },
        { "submit", C_SUBMIT }, { "reset", C_RESET }, { "button", C_BUTTON }, { "hidden", C_HIDDEN },
        { "image", C_IMAGE }, { "file", C_FILE },
    };

    if (t)
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
            if (!strcasecmp(t, types[i].name))
                return types[i].type;
    return C_TEXT;              /* text, search, email, url, tel, number, ... */
}

static void set_value(struct ctl *c, const char *v)
{
    int n = v ? (int)strlen(v) : 0;

    if (n >= c->cap)
        n = c->cap - 1;
    memcpy(c->value, v ? v : "", (size_t)n);
    c->value[n] = 0;
    c->len = n;
}

static struct node *form_of(struct node *n)
{
    for (struct node *a = n->parent; a; a = a->parent)
        if (a->tag == T_FORM)
            return a;
    return NULL;
}

static void collect_options(struct page *p, struct ctl *c)
{
    int n = 0;

    for (struct node *o = c->node; o; o = node_next(o, c->node, false))
        n += o->type == N_ELEM && o->tag == T_OPTION;
    c->opts = n ? arena_alloc(&p->arena, sizeof(struct node *) * (size_t)n) : NULL;
    c->nopts = 0;
    if (!c->opts)
        return;
    for (struct node *o = c->node; o; o = node_next(o, c->node, false)) {
        if (o->type != N_ELEM || o->tag != T_OPTION)
            continue;
        /* The option's label lives in its (otherwise unused) ctext. */
        const char *label = node_attr(o, "label");
        o->ctext = label ? label : node_text(&p->arena, o);
        if (!o->ctext)
            o->ctext = "";
        if (node_attr(o, "selected"))
            c->sel = c->nopts;
        c->opts[c->nopts++] = o;
    }
}

void forms_collect(struct page *p)
{
    struct ctl **tail = &p->ctls;

    for (struct node *n = p->doc; n; n = node_next(n, p->doc, false)) {
        if (n->type != N_ELEM)
            continue;
        int type;
        if (n->tag == T_INPUT)
            type = input_type(node_attr(n, "type"));
        else if (n->tag == T_TEXTAREA)
            type = C_TEXTAREA;
        else if (n->tag == T_SELECT)
            type = C_SELECT;
        else if (n->tag == T_BUTTON) {
            const char *t = node_attr(n, "type");
            type = t && !strcasecmp(t, "reset") ? C_RESET : t && !strcasecmp(t, "button") ? C_BUTTON : C_SUBMIT;
        } else
            continue;
        if (n->ctl) {
            struct ctl *c = n->ctl;
            c->form = form_of(n);
            c->next = NULL;
            if (n->tag == T_BUTTON)
                n->ctext = node_text(&p->arena, n);
            *tail = c;
            tail = &c->next;
            continue;
        }
        struct ctl *c = arena_alloc(&p->arena, sizeof(*c));
        if (!c)
            return;
        c->node = n;
        c->form = form_of(n);
        c->type = (uint8_t)type;
        c->disabled = node_attr(n, "disabled") || type == C_FILE;
        c->checked = node_attr(n, "checked") != NULL;
        c->cap = type == C_TEXTAREA ? 4096 : 512;
        c->value = arena_alloc(&p->arena, (size_t)c->cap);
        if (!c->value)
            return;
        const char *v = node_attr(n, "size");
        c->size = v ? atoi(v) : 0;
        if (type == C_TEXTAREA) {
            v = node_attr(n, "cols");
            c->size = v ? atoi(v) : 20;
            v = node_attr(n, "rows");
            c->rows = v ? atoi(v) : 2;
            c->size = c->size < 1 ? 1 : c->size > 80 ? 80 : c->size;
            c->rows = c->rows < 1 ? 1 : c->rows > 20 ? 20 : c->rows;
            c->init = n->first && n->first->type == N_TEXT ? n->first->text : "";
        } else if (n->tag == T_BUTTON) {
            c->init = node_attr(n, "value");
            n->ctext = node_text(&p->arena, n);     /* its label */
        } else {
            c->init = node_attr(n, "value");
        }
        if (c->size > 60)
            c->size = 60;
        if (type == C_SELECT)
            collect_options(p, c);
        set_value(c, c->init);
        c->init_checked = c->checked;
        c->init_sel = c->sel;
        n->ctl = c;
        *tail = c;
        tail = &c->next;
    }
    *tail=NULL;
}

const char *ctl_label(const struct ctl *c)
{
    const char *v;

    switch (c->type) {
    case C_SELECT:
        return c->nopts ? c->opts[c->sel]->ctext : "";
    case C_RESET:
        return c->len ? c->value : "Reset";
    case C_FILE:
        return "Choose File";
    case C_IMAGE:
        v = node_attr(c->node, "alt");
        return v && *v ? v : "Submit";
    case C_SUBMIT:
    case C_BUTTON:
        if (c->node->tag == T_BUTTON && c->node->ctext)
            return c->node->ctext;
        return c->len ? c->value : c->type == C_SUBMIT ? "Submit" : "";
    default:
        return c->value;
    }
}

void ctl_toggle(struct page *p, struct ctl *c)
{
    if (c->disabled)
        return;
    if (c->type == C_CHECKBOX) {
        c->checked = !c->checked;
    } else if (c->type == C_RADIO) {
        const char *name = node_attr(c->node, "name");
        for (struct ctl *o = p->ctls; o && name; o = o->next) {
            const char *on = node_attr(o->node, "name");
            if (o->type == C_RADIO && o->form == c->form && on && !strcmp(on, name))
                o->checked = false;
        }
        c->checked = true;
    }
}

void forms_reset(struct page *p, struct node *form)
{
    for (struct ctl *c = p->ctls; c; c = c->next) {
        if (c->form != form)
            continue;
        set_value(c, c->init);
        c->checked = c->init_checked;
        c->sel = c->init_sel;
    }
}

static int add_pair(char *out, int len, const char *name, const char *value)
{
    if (len < 0)
        return -1;
    if (len && len + 1 < FORM_DATA_MAX)
        out[len++] = '&';
    len = url_form_encode(out, len, FORM_DATA_MAX, name);
    if (len < 0 || len + 1 >= FORM_DATA_MAX)
        return -1;
    out[len++] = '=';
    return url_form_encode(out, len, FORM_DATA_MAX, value);
}

char *forms_submit(struct page *p, struct node *form, struct ctl *submitter, char **post)
{
    char *data = malloc(FORM_DATA_MAX);
    int len = 0;

    *post = NULL;
    if (!data)
        return NULL;
    data[0] = 0;
    for (struct ctl *c = p->ctls; c && len >= 0; c = c->next) {
        const char *name = node_attr(c->node, "name");
        if (c->form != form || !name || !*name || c->disabled)
            continue;
        switch (c->type) {
        case C_TEXT: case C_PASSWORD: case C_HIDDEN: case C_TEXTAREA:
            len = add_pair(data, len, name, c->value);
            break;
        case C_CHECKBOX: case C_RADIO:
            if (c->checked)
                len = add_pair(data, len, name, c->len ? c->value : "on");
            break;
        case C_SELECT:
            if (c->nopts) {
                const char *v = node_attr(c->opts[c->sel], "value");
                len = add_pair(data, len, name, v ? v : c->opts[c->sel]->ctext);
            }
            break;
        case C_SUBMIT: case C_BUTTON:
            if (c == submitter)
                len = add_pair(data, len, name, c->value);
            break;
        case C_IMAGE:
            if (c == submitter) {
                char nm[128];
                snprintf(nm, sizeof(nm), "%s.x", name);
                len = add_pair(data, len, nm, "0");
                snprintf(nm, sizeof(nm), "%s.y", name);
                len = add_pair(data, len, nm, "0");
            }
            break;
        }
    }
    if (len < 0) {
        free(data);
        return NULL;
    }

    const char *action = form ? node_attr(form, "action") : NULL;
    const char *method = form ? node_attr(form, "method") : NULL;
    char *url = url_resolve(p->base, action && *action ? action : p->url);
    if (!url) {
        free(data);
        return NULL;
    }
    url[strcspn(url, "#")] = 0;
    if (method && !strcasecmp(method, "post")) {
        *post = data;
        return url;
    }
    /* GET: the data replaces the action's query. */
    url[strcspn(url, "?")] = 0;
    size_t ul = strlen(url);
    char *full = malloc(ul + (size_t)len + 2);
    if (full) {
        memcpy(full, url, ul);
        full[ul] = '?';
        memcpy(full + ul + 1, data, (size_t)len + 1);
    }
    free(url);
    free(data);
    return full;
}
