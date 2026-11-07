/*
 * Home-screen layout: which app or folder sits in each toolbar slot and
 * page cell. Pure logic over sb_apps[], so tests/native/layout.c can run
 * it on the host. Included by sb_internal.h after the layout constants.
 *
 * Items reference apps by index (an app keeps its index for the session);
 * the saved form (LAYOUT_PREF) keys them by path. The model keeps items
 * that are hidden (a card app while the card is out) in their place, and
 * every visible position skips them.
 */
#ifndef SB_LAYOUT_H
#define SB_LAYOUT_H

#include <glaze/types.h>

#define LAYOUT_PAGES    6
#define FOLDER_MAX      12          /* a 4x3 grid when open */
#define SB_MAX_FOLDERS  8
#define FOLDER_NAME_MAX 24
#define LAYOUT_PREF     "sb.layout"
#define LAYOUT_TEXT_MAX 4096

/* An app index (>= 0), a folder (ITEM_FOLDER), or nothing. */
typedef int16_t sb_item;
#define ITEM_NONE           ((sb_item)-1)
#define ITEM_FOLDER(f)      ((sb_item)(-2 - (f)))
#define ITEM_IS_FOLDER(it)  ((it) <= -2)
#define ITEM_FOLDER_OF(it)  (-2 - (it))

/* Every page, the toolbar and every folder is a group of items: pages
 * first, then the toolbar, then folders. A folder is in use while it holds
 * apps (two or more, once layout_tidy has run). */
#define G_DOCK          LAYOUT_PAGES
#define G_FOLDER(f)     (LAYOUT_PAGES + 1 + (f))
#define LAYOUT_GROUPS   G_FOLDER(SB_MAX_FOLDERS)

struct sb_layout {
    int8_t npages;                  /* at least 1 */
    int8_t n[LAYOUT_GROUPS];
    sb_item item[LAYOUT_GROUPS][PER_PAGE];
    char name[SB_MAX_FOLDERS][FOLDER_NAME_MAX];
};

static inline int layout_folder_size(const struct sb_layout *L, int f)
{
    return f >= 0 && f < SB_MAX_FOLDERS ? L->n[G_FOLDER(f)] : 0;
}

/* Where an item is in the model: group is the page or folder number,
 * index its position among all (also hidden) items there. */
enum { LOC_NONE, LOC_PAGE, LOC_DOCK, LOC_FOLDER };
struct sb_loc {
    int8_t where, group, index;
};

/* Where a dragged item goes. cell is a visible position (past the end
 * appends); DROP_MERGE puts an app into `onto`, an app (making a folder)
 * or a folder. */
enum { DROP_NONE, DROP_PAGE, DROP_DOCK, DROP_FOLDER, DROP_MERGE };
struct sb_drop {
    int8_t kind, group, cell;
    sb_item onto;
};

extern struct sb_layout sb_layout;
extern bool sb_layout_dirty;        /* changed since the last save */

bool layout_item_shown(const struct sb_layout *L, sb_item it);
/* The items of a group (LOC_PAGE/LOC_DOCK/LOC_FOLDER) and their count. */
const sb_item *layout_group(const struct sb_layout *L, int where, int group, int *n);
/* Shown items of a group, in order; returns how many. */
int layout_visible(const struct sb_layout *L, int where, int group, sb_item *out, int max);
/* An app's folder, or -1. */
int layout_folder_of(const struct sb_layout *L, int app);

/* Built-ins' dock flag picks the toolbar, everything else fills pages. */
void layout_default(struct sb_layout *L);
bool layout_find(const struct sb_layout *L, sb_item it, struct sb_loc *loc);
sb_item layout_at(const struct sb_layout *L, const struct sb_loc *loc);
/* Moves the item at `from` as `to` says; false (and L unchanged) when it
 * can't go there: a full dock or folder, a folder into a folder. */
bool layout_move(struct sb_layout *L, const struct sb_loc *from, const struct sb_drop *to);
/* Folders left with one app become that app, empty ones go; with pages,
 * empty pages go too (at least one stays). */
void layout_tidy(struct sb_layout *L, bool pages);
/* An uninstalled app leaves the layout. */
bool layout_forget_app(struct sb_layout *L, int app);
/* Drops apps that are gone and duplicates, appends shown apps the layout
 * lacks. True if anything changed. */
bool layout_sync(struct sb_layout *L);
/* A new folder's default name: "Folder", "Folder 2", ... */
void layout_folder_name(const struct sb_layout *L, char *out, int len);
bool layout_rename(struct sb_layout *L, int folder, const char *name);

/* LAYOUT_PREF text: length written (0 if it doesn't fit). */
int layout_format(const struct sb_layout *L, char *buf, int len);
/* Unknown paths are dropped (layout_sync appends the app if it returns). */
bool layout_parse(struct sb_layout *L, const char *text, int len);

#endif
