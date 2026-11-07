/* SpringBoard's home layout model (servers/springboard/layout.c): moves,
 * the toolbar, folders, syncing with the app list, and the saved text. */
#include "../../servers/springboard/sb_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct sb_app_info sb_apps[SB_MAX_APPS];
int sb_num_apps;

static void add_app(const char *path, bool dock)
{
    struct sb_app_info *a = &sb_apps[sb_num_apps++];

    memset(a, 0, sizeof(*a));
    snprintf(a->path, sizeof(a->path), "%s", path);
    snprintf(a->label, sizeof(a->label), "%s", path);
    a->dock = dock;
    a->present = true;
}

static int vis(const struct sb_layout *L, int where, int group, sb_item *out)
{
    return layout_visible(L, where, group, out, PER_PAGE);
}

static struct sb_loc at(const struct sb_layout *L, sb_item it)
{
    struct sb_loc loc;

    assert(layout_find(L, it, &loc));
    return loc;
}

static bool move(struct sb_layout *L, sb_item it, struct sb_drop d)
{
    struct sb_loc from = at(L, it);

    return layout_move(L, &from, &d);
}

/* Every shown app appears exactly once, and folders hold two or more. */
static void check(const struct sb_layout *L)
{
    int seen[SB_MAX_APPS] = { 0 };

    assert(L->npages >= 1 && L->npages <= LAYOUT_PAGES);
    assert(L->n[G_DOCK] <= DOCK_SLOTS);
    for (int g = 0; g < LAYOUT_GROUPS; g++) {
        int n = g < G_DOCK && g >= L->npages ? 0 : L->n[g];
        assert(n >= 0 && n <= PER_PAGE);
        for (int i = 0; i < n; i++) {
            sb_item it = L->item[g][i];
            if (ITEM_IS_FOLDER(it)) {
                assert(g <= G_DOCK);
                assert(layout_folder_size(L, ITEM_FOLDER_OF(it)) >= 2);
            } else {
                assert(it >= 0 && it < sb_num_apps);
                seen[it]++;
            }
        }
    }
    for (int i = 0; i < sb_num_apps; i++)
        assert(seen[i] == (sb_apps[i].path[0] ? 1 : 0));
}

int main(void)
{
    static char text[LAYOUT_TEXT_MAX];
    struct sb_layout L, M;
    sb_item it[PER_PAGE];
    int n;

    /* 14 apps: 2 for the toolbar, 12 filling page 0, 1 on page 1. */
    for (int i = 0; i < 14; i++) {
        char p[16];
        snprintf(p, sizeof(p), "bin/a%d", i);
        add_app(p, i == 5 || i == 6);
    }
    layout_default(&L);
    check(&L);
    assert(L.npages == 1 && L.n[0] == 12 && L.n[G_DOCK] == 2);
    assert(L.item[G_DOCK][0] == 5 && L.item[G_DOCK][1] == 6);
    add_app("bin/a14", false);
    assert(layout_sync(&L));
    check(&L);
    assert(L.npages == 2 && L.n[1] == 1 && L.item[1][0] == 14);
    assert(!layout_sync(&L));

    /* Moving on a page; a full page passes its last item on. */
    assert(move(&L, 0, (struct sb_drop){ DROP_PAGE, 0, 3, ITEM_NONE }));
    assert(L.item[0][3] == 0 && L.item[0][0] == 1);
    assert(move(&L, 14, (struct sb_drop){ DROP_PAGE, 0, 0, ITEM_NONE }));
    assert(L.item[0][0] == 14 && L.n[0] == 12 && L.item[1][0] == 13 && L.n[1] == 1);
    check(&L);
    /* Past the end of a page appends; a new page past the last. */
    assert(move(&L, 2, (struct sb_drop){ DROP_PAGE, 1, PER_PAGE, ITEM_NONE }));
    assert(L.item[1][1] == 2);
    assert(move(&L, 3, (struct sb_drop){ DROP_PAGE, 2, 0, ITEM_NONE }));
    assert(L.npages == 3 && L.item[2][0] == 3);
    assert(!move(&L, 3, (struct sb_drop){ DROP_PAGE, LAYOUT_PAGES, 0, ITEM_NONE }));
    check(&L);

    /* Toolbar: up to DOCK_SLOTS, then refused (and L unchanged). */
    assert(move(&L, 1, (struct sb_drop){ DROP_DOCK, 0, 0, ITEM_NONE }));
    assert(move(&L, 4, (struct sb_drop){ DROP_DOCK, 0, 9, ITEM_NONE }));
    assert(L.n[G_DOCK] == 4 && L.item[G_DOCK][0] == 1 && L.item[G_DOCK][3] == 4);
    M = L;
    assert(!move(&L, 7, (struct sb_drop){ DROP_DOCK, 0, 1, ITEM_NONE }));
    assert(!memcmp(&L, &M, sizeof(L)));
    assert(move(&L, 4, (struct sb_drop){ DROP_DOCK, 0, 0, ITEM_NONE }));     /* reorder within */
    assert(L.item[G_DOCK][0] == 4);
    check(&L);

    /* Folders: merging makes one where the target was. */
    struct sb_loc t8 = at(&L, 8);
    assert(move(&L, 7, (struct sb_drop){ .kind = DROP_MERGE, .onto = 8 }));
    struct sb_loc f0 = at(&L, ITEM_FOLDER(0));
    assert(f0.where == t8.where && f0.group == t8.group);
    assert(layout_folder_size(&L, 0) == 2 && !strcmp(L.name[0], "Folder"));
    assert(layout_folder_of(&L, 7) == 0 && layout_folder_of(&L, 8) == 0);
    assert(move(&L, 9, (struct sb_drop){ .kind = DROP_MERGE, .onto = ITEM_FOLDER(0) }));
    assert(layout_folder_size(&L, 0) == 3);
    assert(!move(&L, ITEM_FOLDER(0), (struct sb_drop){ .kind = DROP_MERGE, .onto = 10 }));
    assert(!move(&L, ITEM_FOLDER(0), (struct sb_drop){ DROP_FOLDER, 0, 0, ITEM_NONE }));
    assert(move(&L, 10, (struct sb_drop){ .kind = DROP_MERGE, .onto = 11 }));
    assert(!strcmp(L.name[1], "Folder 2"));
    /* A folder in the toolbar. */
    assert(move(&L, ITEM_FOLDER(1), (struct sb_drop){ DROP_DOCK, 0, 0, ITEM_NONE }) == false);
    assert(move(&L, 6, (struct sb_drop){ DROP_PAGE, 0, 0, ITEM_NONE }));
    assert(move(&L, ITEM_FOLDER(1), (struct sb_drop){ DROP_DOCK, 0, 0, ITEM_NONE }));
    assert(L.item[G_DOCK][0] == ITEM_FOLDER(1));
    check(&L);
    /* Reordering inside, then out; one app left dissolves the folder. */
    assert(move(&L, 9, (struct sb_drop){ DROP_FOLDER, 0, 0, ITEM_NONE }));
    assert(L.item[G_FOLDER(0)][0] == 9);
    assert(move(&L, 9, (struct sb_drop){ DROP_PAGE, 1, 0, ITEM_NONE }));
    assert(layout_folder_size(&L, 0) == 2);
    assert(move(&L, 7, (struct sb_drop){ DROP_PAGE, 1, 0, ITEM_NONE }));
    assert(layout_folder_size(&L, 0) == 0 && !layout_find(&L, ITEM_FOLDER(0), &f0));
    assert(at(&L, 8).where == t8.where && at(&L, 8).group == t8.group);
    check(&L);

    /* Names: trimmed, printable, default when empty. */
    assert(layout_rename(&L, 1, "  Games  "));
    assert(!strcmp(L.name[1], "Games"));
    assert(layout_rename(&L, 1, ""));
    assert(!strcmp(L.name[1], "Folder"));
    assert(layout_rename(&L, 1, "Tools"));
    assert(!layout_rename(&L, 0, "Nope"));

    /* Hidden apps keep their place; visible cells skip them. */
    sb_apps[13].present = false;
    struct sb_loc h = at(&L, 13);
    n = vis(&L, h.where, h.group, it);
    for (int i = 0; i < n; i++)
        assert(it[i] != 13);
    assert(!layout_sync(&L));
    sb_apps[13].present = true;

    /* Saved text round trip. */
    n = layout_format(&L, text, sizeof(text));
    assert(n > 0 && !strncmp(text, "glaze-layout 1\ndock\n", 20));
    assert(strstr(text, "folder Tools\napp bin/a11\napp bin/a10\nend\n"));
    assert(layout_parse(&M, text, n));
    check(&M);
    static char again[LAYOUT_TEXT_MAX];
    assert(layout_format(&M, again, sizeof(again)) == n && !memcmp(text, again, (size_t)n));
    assert(!strcmp(M.name[layout_folder_of(&M, 10)], "Tools"));
    assert(layout_format(&L, text, 40) == 0);   /* doesn't fit */

    /* Uninstalls and unknown or damaged text. */
    assert(layout_forget_app(&L, 10));
    assert(layout_folder_of(&L, 11) < 0);      /* its folder dissolved */
    sb_apps[10].path[0] = 0;
    check(&L);
    sb_apps[12].path[0] = 0;
    assert(layout_sync(&L));
    check(&L);
    assert(!layout_parse(&M, "nonsense", 8));
    static const char junk[] = "glaze-layout 1\nend\napp bin/a0\nfolder X\ndock\napp bin/zz\napp bin/a1\n"
                               "folder Empty\nend\nfolder Lonely\napp bin/a2\nend\npage\napp bin/a3\napp bin/a3\n"
                               "page\npage\nfolder A\napp bin/a4\napp bin/a5\nfolder B\napp bin/a6\nend\n";
    assert(layout_parse(&M, junk, (int)strlen(junk)));
    layout_sync(&M);
    check(&M);
    assert(M.item[G_DOCK][0] == 1 && M.item[G_DOCK][1] == 2 && M.n[G_DOCK] == 2);

    /* Many random moves keep every invariant. */
    unsigned seed = 1;
    for (int k = 0; k < 20000; k++) {
        seed = seed * 1103515245u + 12345u;
        sb_item src = (sb_item)((seed >> 8) % (unsigned)sb_num_apps);
        if ((seed >> 20) % 5 == 0) {
            int f = layout_folder_of(&L, src);
            if (f >= 0)
                src = ITEM_FOLDER(f);
        }
        struct sb_loc from;
        if (!layout_find(&L, src, &from))
            continue;
        struct sb_drop d = { (int8_t)(1 + (seed >> 12) % 4), (int8_t)((seed >> 16) % 4),
                             (int8_t)((seed >> 24) % 14), (sb_item)((seed >> 4) % (unsigned)sb_num_apps) };
        if (d.kind == DROP_MERGE && (seed & 1)) {
            int f = layout_folder_of(&L, d.onto);
            if (f >= 0)
                d.onto = ITEM_FOLDER(f);
        }
        M = L;
        if (!layout_move(&L, &from, &d))
            assert(!memcmp(&L, &M, sizeof(L)));
        if (k % 97 == 0)
            layout_tidy(&L, true);
        check(&L);
    }
    n = layout_format(&L, text, sizeof(text));
    assert(n > 0 && layout_parse(&M, text, n));
    check(&M);
    printf("layout: ALL PASS\n");
    return 0;
}
