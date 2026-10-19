/*
 * SpringBoard's badges and notifications (see glaze/sb.h): the per-app
 * badge counts, the scheduled notes, and the notes delivered while locked.
 * Badges and scheduled notes persist in preferences, keyed by the app's
 * binary path so reordering sb_apps[] keeps them with their apps.
 */

#include "sb_internal.h"

#include <glaze/prefs.h>
#include <string.h>

uint16_t sb_badges[SB_MAX_APPS];
struct sb_note sb_delivered[SB_LOCK_NOTES];
int sb_num_delivered;

static struct sb_note pending[SB_MAX_NOTES];
static int num_pending;

struct badge_rec {
    char path[48];
    uint32_t count;
};

struct note_rec {
    char path[48];
    uint32_t id, when, badge;
    char title[32];
    char body[96];
};

int notify_app_for_path(const char *path)
{
    for (int i = 0; i < sb_num_apps; i++) {
        if (!strncmp(sb_apps[i].path, path, sizeof(((struct badge_rec *)0)->path)))
            return i;
    }
    return -1;
}

static void save_badges(void)
{
    struct badge_rec recs[SB_MAX_APPS];
    int n = 0;

    memset(recs, 0, sizeof(recs));
    for (int i = 0; i < sb_num_apps; i++) {
        if (sb_badges[i]) {
            strlcpy(recs[n].path, sb_apps[i].path, sizeof(recs[n].path));
            recs[n++].count = sb_badges[i];
        }
    }
    if (n)
        pref_put("sb.badges", recs, (uint32_t)(n * sizeof(recs[0])));
    else
        pref_delete("sb.badges");
}

static void save_notes(void)
{
    static struct note_rec recs[SB_MAX_NOTES];

    memset(recs, 0, sizeof(recs));
    for (int i = 0; i < num_pending; i++) {
        const struct sb_note *p = &pending[i];
        strlcpy(recs[i].path, sb_apps[p->app].path, sizeof(recs[i].path));
        recs[i].id = p->id;
        recs[i].when = p->when;
        recs[i].badge = p->badge;
        memcpy(recs[i].title, p->title, sizeof(recs[i].title));
        memcpy(recs[i].body, p->body, sizeof(recs[i].body));
    }
    if (num_pending)
        pref_put("sb.notes", recs, (uint32_t)(num_pending * sizeof(recs[0])));
    else
        pref_delete("sb.notes");
}

void notify_load(void)
{
    static struct note_rec notes[SB_MAX_NOTES];
    struct badge_rec badges[SB_MAX_APPS];
    int len;

    len = pref_get("sb.badges", badges, sizeof(badges));
    for (int i = 0; len > 0 && i < len / (int)sizeof(badges[0]); i++) {
        badges[i].path[sizeof(badges[i].path) - 1] = 0;
        int app = notify_app_for_path(badges[i].path);
        if (app >= 0)
            sb_badges[app] = (uint16_t)(badges[i].count > 0xFFFF ? 0xFFFF : badges[i].count);
    }
    len = pref_get("sb.notes", notes, sizeof(notes));
    for (int i = 0; len > 0 && i < len / (int)sizeof(notes[0]); i++) {
        struct note_rec *r = &notes[i];
        r->path[sizeof(r->path) - 1] = 0;
        r->title[sizeof(r->title) - 1] = 0;
        r->body[sizeof(r->body) - 1] = 0;
        int app = notify_app_for_path(r->path);
        if (app < 0)
            continue;
        struct sb_note *p = &pending[num_pending++];
        p->app = app;
        p->id = r->id;
        p->when = r->when;
        p->badge = r->badge;
        memcpy(p->title, r->title, sizeof(p->title));
        memcpy(p->body, r->body, sizeof(p->body));
    }
}

void notify_set_badge(int app, uint32_t count)
{
    count = count > 0xFFFF ? 0xFFFF : count;
    if (sb_badges[app] == count)
        return;
    sb_badges[app] = (uint16_t)count;
    save_badges();
}

void notify_add_badge(int app, uint32_t n)
{
    if (n)
        notify_set_badge(app, sb_badges[app] + n);
}

static int find(int app, uint32_t id)
{
    for (int i = 0; i < num_pending; i++) {
        if (pending[i].app == app && pending[i].id == id)
            return i;
    }
    return -1;
}

static void remove_at(int i)
{
    pending[i] = pending[--num_pending];
}

int notify_post(int app, uint32_t id, uint32_t when, uint32_t badge, const char *title, const char *body)
{
    int i = find(app, id);

    if (i < 0) {
        if (num_pending == SB_MAX_NOTES)
            return GZ_ENOSPC;
        i = num_pending++;
    }
    struct sb_note *p = &pending[i];
    memset(p, 0, sizeof(*p));
    p->app = app;
    p->id = id;
    p->when = when;
    p->badge = badge;
    strlcpy(p->title, title, sizeof(p->title));
    strlcpy(p->body, body, sizeof(p->body));
    save_notes();
    return 0;
}

int notify_cancel(int app, uint32_t id)
{
    int i = find(app, id);

    if (i < 0)
        return GZ_ENOENT;
    remove_at(i);
    save_notes();
    return 0;
}

uint32_t notify_pending(int app, uint32_t id)
{
    int i = find(app, id);

    return i < 0 ? 0 : pending[i].when ? pending[i].when : 1;
}

int notify_next_ms(uint32_t now)
{
    int ms = -1;

    for (int i = 0; i < num_pending; i++) {
        uint32_t w = pending[i].when;
        int t = w <= now ? 0 : w - now > 3600 ? 3600000 : (int)(w - now) * 1000;
        if (ms < 0 || t < ms)
            ms = t;
    }
    return ms;
}

bool notify_pop_due(uint32_t now, struct sb_note *out)
{
    int best = -1;

    for (int i = 0; i < num_pending; i++) {
        if (pending[i].when <= now && (best < 0 || pending[i].when < pending[best].when))
            best = i;
    }
    if (best < 0)
        return false;
    *out = pending[best];
    remove_at(best);
    save_notes();
    return true;
}

void notify_deliver_locked(const struct sb_note *n)
{
    /* Newest first; the oldest falls off the end. */
    if (sb_num_delivered < SB_LOCK_NOTES)
        sb_num_delivered++;
    memmove(&sb_delivered[1], &sb_delivered[0], (size_t)(sb_num_delivered - 1) * sizeof(sb_delivered[0]));
    sb_delivered[0] = *n;
}

void notify_clear_delivered(void)
{
    sb_num_delivered = 0;
}
