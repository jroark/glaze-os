/*
 * Home-screen layout model (layout.h): pages, toolbar and folders, the
 * edits a drag makes, and the LAYOUT_PREF text that keeps them.
 */

#include "sb_internal.h"

#include <stdio.h>
#include <string.h>

struct sb_layout sb_layout;
bool sb_layout_dirty;

static bool app_valid(int app)
{
    return app >= 0 && app < sb_num_apps && sb_apps[app].path[0];
}

static int gid(int where, int group)
{
    return where == LOC_DOCK ? G_DOCK : where == LOC_FOLDER ? G_FOLDER(group) : group;
}

static int capacity(int g)
{
    return g == G_DOCK ? DOCK_SLOTS : PER_PAGE;
}

static struct sb_loc loc_of(int g, int index)
{
    if (g < G_DOCK)
        return (struct sb_loc){ LOC_PAGE, (int8_t)g, (int8_t)index };
    if (g == G_DOCK)
        return (struct sb_loc){ LOC_DOCK, 0, (int8_t)index };
    return (struct sb_loc){ LOC_FOLDER, (int8_t)(g - G_FOLDER(0)), (int8_t)index };
}

/* Pages past the last are empty, as are free folders. */
static int count(const struct sb_layout *L, int g)
{
    return g < G_DOCK && g >= L->npages ? 0 : L->n[g];
}

bool layout_item_shown(const struct sb_layout *L, sb_item it)
{
    if (!ITEM_IS_FOLDER(it))
        return app_valid(it) && sb_app_shown(it);
    int g = G_FOLDER(ITEM_FOLDER_OF(it));
    for (int i = 0; i < L->n[g]; i++) {
        if (layout_item_shown(L, L->item[g][i]))
            return true;
    }
    return false;
}

const sb_item *layout_group(const struct sb_layout *L, int where, int group, int *n)
{
    int g = gid(where, group);

    if (where == LOC_NONE || group < 0 || g >= LAYOUT_GROUPS || (where == LOC_PAGE && group >= LAYOUT_PAGES)) {
        *n = 0;
        return NULL;
    }
    *n = count(L, g);
    return L->item[g];
}

int layout_visible(const struct sb_layout *L, int where, int group, sb_item *out, int max)
{
    int n, v = 0;
    const sb_item *items = layout_group(L, where, group, &n);

    for (int i = 0; i < n && v < max; i++) {
        if (layout_item_shown(L, items[i]))
            out[v++] = items[i];
    }
    return v;
}

static bool find(const struct sb_layout *L, sb_item it, int *g, int *index)
{
    for (*g = 0; *g < LAYOUT_GROUPS; (*g)++) {
        for (*index = 0; *index < count(L, *g); (*index)++) {
            if (L->item[*g][*index] == it)
                return true;
        }
    }
    return false;
}

bool layout_find(const struct sb_layout *L, sb_item it, struct sb_loc *loc)
{
    int g, i;

    if (!find(L, it, &g, &i)) {
        loc->where = LOC_NONE;
        return false;
    }
    *loc = loc_of(g, i);
    return true;
}

int layout_folder_of(const struct sb_layout *L, int app)
{
    struct sb_loc loc;

    return layout_find(L, (sb_item)app, &loc) && loc.where == LOC_FOLDER ? loc.group : -1;
}

sb_item layout_at(const struct sb_layout *L, const struct sb_loc *loc)
{
    int n;
    const sb_item *items = layout_group(L, loc->where, loc->group, &n);

    return items && loc->index >= 0 && loc->index < n ? items[loc->index] : ITEM_NONE;
}

static void insert(struct sb_layout *L, int g, int index, sb_item it)
{
    memmove(&L->item[g][index + 1], &L->item[g][index], (size_t)(L->n[g] - index) * sizeof(sb_item));
    L->item[g][index] = it;
    L->n[g]++;
}

static void remove_at(struct sb_layout *L, int g, int index)
{
    L->n[g]--;
    memmove(&L->item[g][index], &L->item[g][index + 1], (size_t)(L->n[g] - index) * sizeof(sb_item));
}

/* Model index of the `cell`th shown item, or the end. */
static int model_index(const struct sb_layout *L, int g, int cell)
{
    int v = 0, i;

    for (i = 0; i < count(L, g); i++) {
        if (layout_item_shown(L, L->item[g][i]) && v++ >= cell)
            break;
    }
    return i;
}

/* A full page passes its last item on to the next, creating it. */
static bool insert_page(struct sb_layout *L, int page, int index, sb_item it)
{
    for (; page < LAYOUT_PAGES; page++, index = 0) {
        while (L->npages <= page)
            L->n[L->npages++] = 0;
        if (index >= PER_PAGE)
            continue;                   /* after a full page: the next one's start */
        sb_item last = L->item[page][PER_PAGE - 1];
        bool full = L->n[page] == PER_PAGE;
        if (full)
            L->n[page]--;
        insert(L, page, index, it);
        if (!full)
            return true;
        it = last;
    }
    return false;
}

static void append_page(struct sb_layout *L, sb_item it)
{
    int p = L->npages - 1;

    while (p < LAYOUT_PAGES && count(L, p) == PER_PAGE)
        p++;
    if (p == LAYOUT_PAGES)
        for (p = 0; p < L->npages && L->n[p] == PER_PAGE; p++)
            ;
    if (p < LAYOUT_PAGES)
        insert_page(L, p, count(L, p), it);
}

static void clear(struct sb_layout *L)
{
    memset(L, 0, sizeof(*L));
    L->npages = 1;
}

void layout_default(struct sb_layout *L)
{
    clear(L);
    for (int i = 0; i < sb_num_apps; i++) {
        if (!sb_app_shown(i))
            continue;
        if (sb_apps[i].dock && L->n[G_DOCK] < DOCK_SLOTS)
            insert(L, G_DOCK, L->n[G_DOCK], (sb_item)i);
        else
            append_page(L, (sb_item)i);
    }
}

void layout_folder_name(const struct sb_layout *L, char *out, int len)
{
    for (int k = 1; k <= SB_MAX_FOLDERS + 1; k++) {
        bool taken = false;
        snprintf(out, (size_t)len, k == 1 ? "Folder" : "Folder %d", k);
        for (int f = 0; f < SB_MAX_FOLDERS; f++)
            taken |= L->n[G_FOLDER(f)] && !strcmp(L->name[f], out);
        if (!taken)
            return;
    }
}

/* A folder nothing holds, named; -1 if all are in use. */
static int new_folder(struct sb_layout *L)
{
    int g, i;

    for (int f = 0; f < SB_MAX_FOLDERS; f++) {
        if (!L->n[G_FOLDER(f)] && !find(L, ITEM_FOLDER(f), &g, &i)) {
            layout_folder_name(L, L->name[f], FOLDER_NAME_MAX);
            return f;
        }
    }
    return -1;
}

bool layout_move(struct sb_layout *L, const struct sb_loc *from, const struct sb_drop *to)
{
    struct sb_layout t = *L;
    sb_item it = layout_at(&t, from);
    int g = gid(to->kind == DROP_DOCK ? LOC_DOCK : LOC_FOLDER, to->group), ag, ai;

    if (it == ITEM_NONE)
        return false;
    remove_at(&t, gid(from->where, from->group), from->index);
    switch (to->kind) {
    case DROP_PAGE:
        if (to->group < 0 || to->group >= LAYOUT_PAGES ||
            !insert_page(&t, to->group, model_index(&t, to->group, to->cell), it))
            return false;
        break;
    case DROP_FOLDER:
        if (to->group < 0 || to->group >= SB_MAX_FOLDERS || !t.n[g] || ITEM_IS_FOLDER(it))
            return false;
        /* fall through */
    case DROP_DOCK:
        if (t.n[g] >= capacity(g))
            return false;
        insert(&t, g, model_index(&t, g, to->cell), it);
        break;
    case DROP_MERGE:
        if (ITEM_IS_FOLDER(it) || to->onto == it || !find(&t, to->onto, &ag, &ai) || ag > G_DOCK)
            return false;
        if (ITEM_IS_FOLDER(to->onto)) {
            g = G_FOLDER(ITEM_FOLDER_OF(to->onto));
            if (t.n[g] >= FOLDER_MAX)
                return false;
        } else {
            int f = new_folder(&t);
            if (f < 0)
                return false;
            g = G_FOLDER(f);
            t.item[g][0] = to->onto;
            t.n[g] = 1;
            t.item[ag][ai] = ITEM_FOLDER(f);
        }
        insert(&t, g, t.n[g], it);
        break;
    default:
        return false;
    }
    layout_tidy(&t, false);
    *L = t;
    sb_layout_dirty = true;
    return true;
}

void layout_tidy(struct sb_layout *L, bool pages)
{
    for (int f = 0; f < SB_MAX_FOLDERS; f++) {
        int g = G_FOLDER(f), ag, ai;
        bool placed = find(L, ITEM_FOLDER(f), &ag, &ai);
        if (placed && L->n[g] > 1)
            continue;
        if (placed && L->n[g] == 1)
            L->item[ag][ai] = L->item[g][0];    /* one app left: just the app */
        else if (placed)
            remove_at(L, ag, ai);
        L->n[g] = 0;                    /* also a folder nothing holds */
    }
    for (int p = 0; pages && p < L->npages && L->npages > 1;) {
        if (L->n[p]) {
            p++;
            continue;
        }
        int rest = L->npages - p - 1;
        memmove(&L->item[p], &L->item[p + 1], (size_t)rest * sizeof(L->item[0]));
        memmove(&L->n[p], &L->n[p + 1], (size_t)rest);
        L->n[--L->npages] = 0;
    }
}

/* Removes the apps `drop` picks; folders left too small go too. */
static bool filter(struct sb_layout *L, bool (*drop)(sb_item it, void *arg), void *arg)
{
    bool changed = false;

    for (int g = 0; g < LAYOUT_GROUPS; g++) {
        for (int i = 0; i < count(L, g);) {
            if (!ITEM_IS_FOLDER(L->item[g][i]) && drop(L->item[g][i], arg))
                remove_at(L, g, i), changed = true;
            else
                i++;
        }
    }
    if (changed)
        layout_tidy(L, false);
    sb_layout_dirty |= changed;
    return changed;
}

static bool is_app(sb_item it, void *arg)
{
    return it == *(int *)arg;
}

bool layout_forget_app(struct sb_layout *L, int app)
{
    return filter(L, is_app, &app);
}

static bool gone_or_seen(sb_item it, void *arg)
{
    bool *seen = arg;

    if (!app_valid(it) || seen[it])
        return true;
    seen[it] = true;
    return false;
}

bool layout_sync(struct sb_layout *L)
{
    bool seen[SB_MAX_APPS] = { 0 };
    bool changed = filter(L, gone_or_seen, seen);

    for (int i = 0; i < sb_num_apps; i++) {
        if (!seen[i] && sb_app_shown(i)) {
            append_page(L, (sb_item)i);
            changed = true;
        }
    }
    sb_layout_dirty |= changed;
    return changed;
}

/* Printable ASCII, trimmed; an empty name gets the default. */
bool layout_rename(struct sb_layout *L, int folder, const char *name)
{
    char clean[FOLDER_NAME_MAX];
    int n = 0;

    if (!layout_folder_size(L, folder))
        return false;
    for (; *name && n < FOLDER_NAME_MAX - 1; name++) {
        if (*name > ' ' - (n > 0) && *name < 0x7f)     /* no leading spaces */
            clean[n++] = *name;
    }
    while (n && clean[n - 1] == ' ')
        n--;
    clean[n] = 0;
    if (!n) {
        L->name[folder][0] = 0;
        layout_folder_name(L, clean, sizeof(clean));
    }
    if (strcmp(L->name[folder], clean)) {
        strlcpy(L->name[folder], clean, sizeof(L->name[folder]));
        sb_layout_dirty = true;
    }
    return true;
}

/* ---- LAYOUT_PREF text ---------------------------------------------------
 *
 *   glaze-layout 1
 *   dock
 *   app bin/notes
 *   page
 *   app bin/calendar
 *   folder Games
 *   app /Applications/chess.app
 *   end
 */

#define HEADER "glaze-layout 1\n"

struct out {
    char *buf;
    int len, pos;
};

static void put(struct out *o, const char *a, const char *b)
{
    int n = snprintf(o->buf + o->pos, (size_t)(o->len - o->pos), "%s%s\n", a, b);

    o->pos = n < 0 || o->pos + n >= o->len ? o->len : o->pos + n;
}

static void put_group(struct out *o, const struct sb_layout *L, int g)
{
    for (int i = 0; i < count(L, g); i++) {
        sb_item it = L->item[g][i];
        if (ITEM_IS_FOLDER(it)) {
            put(o, "folder ", L->name[ITEM_FOLDER_OF(it)]);
            put_group(o, L, G_FOLDER(ITEM_FOLDER_OF(it)));
            put(o, "end", "");
        } else if (app_valid(it)) {
            put(o, "app ", sb_apps[it].path);
        }
    }
}

int layout_format(const struct sb_layout *L, char *buf, int len)
{
    struct out o = { buf, len, 0 };

    put(&o, HEADER "dock", "");
    put_group(&o, L, G_DOCK);
    for (int p = 0; p < L->npages; p++) {
        put(&o, "page", "");
        put_group(&o, L, p);
    }
    return o.pos < len ? o.pos : 0;
}

static int app_for_path(const char *path)
{
    for (int i = 0; i < sb_num_apps; i++) {
        if (sb_apps[i].path[0] && !strcmp(sb_apps[i].path, path))
            return i;
    }
    return -1;
}

bool layout_parse(struct sb_layout *L, const char *text, int len)
{
    int hl = (int)strlen(HEADER), g = -1, outer = -1;

    if (len < hl || memcmp(text, HEADER, (size_t)hl))
        return false;
    clear(L);
    L->npages = 0;
    for (int pos = hl; pos < len;) {
        char line[80];
        int end = pos;
        while (end < len && text[end] != '\n')
            end++;
        int n = end - pos < (int)sizeof(line) - 1 ? end - pos : (int)sizeof(line) - 1;
        memcpy(line, text + pos, (size_t)n);
        line[n] = 0;
        pos = end + 1;

        if (!strcmp(line, "dock") || !strcmp(line, "page")) {
            g = line[0] == 'd' ? G_DOCK : L->npages < LAYOUT_PAGES ? L->npages++ : -1;
            outer = -1;
        } else if (!strcmp(line, "end")) {
            g = outer;
            outer = -1;
        } else if (!strncmp(line, "folder ", 7) && g >= 0 && outer < 0 && L->n[g] < capacity(g)) {
            int f = new_folder(L);
            if (f < 0)
                continue;
            insert(L, g, L->n[g], ITEM_FOLDER(f));
            strlcpy(L->name[f], line + 7, FOLDER_NAME_MAX);
            outer = g;
            g = G_FOLDER(f);
            L->n[g] = 0;
        } else if (!strncmp(line, "app ", 4) && g >= 0 && L->n[g] < capacity(g)) {
            int app = app_for_path(line + 4);
            if (app >= 0)               /* else gone, or on a card that is out */
                insert(L, g, L->n[g], (sb_item)app);
        }
    }
    if (!L->npages)
        L->npages = 1;
    layout_tidy(L, true);
    return true;
}
