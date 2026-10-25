#include "display.h"
#include <glaze/fs.h>
#include <glaze/prefs.h>
#include <glaze/sb.h>
#include <glaze/sys.h>

void sb_display_cancel(struct sb_display *s)
{
    if (s->owner) {
        gz_display_set(s->saved.brightness, s->saved.contrast);
        s->current = s->saved;
        s->owner = 0;
    }
}

void sb_display_load(struct sb_display *s)
{
    struct { struct display_settings v; uint32_t extra; } record;
    s->saved = (struct display_settings)DISPLAY_DEFAULTS;
    if (pref_get(DISPLAY_PREF, &record, sizeof(record)) == sizeof(record.v) && display_valid(&record.v))
        s->saved = record.v;
    s->current = s->saved;
    s->owner = 0;
    gz_display_set(s->current.brightness, s->current.contrast);
}

int sb_display_request(struct sb_display *s, uint32_t op, int pid, int brightness, int contrast)
{
    struct display_settings next = { DISPLAY_VERSION, brightness, contrast };
    if (op == SB_DISPLAY_GET)
        return 0;
    if (s->owner && s->owner != pid)
        return GZ_EPERM;
    if (op == SB_DISPLAY_CANCEL) {
        sb_display_cancel(s);
        return 0;
    }
    if (op == SB_DISPLAY_RESET) {
        int rv = pref_delete(DISPLAY_PREF);
        if (rv < 0 && rv != GZ_ENOENT)
            return rv;
        s->saved = (struct display_settings)DISPLAY_DEFAULTS;
        s->current = s->saved;
        s->owner = 0;
        return gz_display_set(s->current.brightness, s->current.contrast);
    }
    if ((op != SB_DISPLAY_PREVIEW && op != SB_DISPLAY_SAVE) || !display_valid(&next))
        return GZ_EINVAL;
    int rv = gz_display_set(brightness, contrast);
    if (rv < 0)
        return rv;
    s->current = next;
    s->owner = pid;
    if (op == SB_DISPLAY_PREVIEW)
        return 0;
    const char *tmp = PREFS_DIR "/" DISPLAY_PREF ".tmp";
    rv = fs_write_file(tmp, &next, sizeof(next));
    if (rv == GZ_ENOENT && fs_mkdirs(PREFS_DIR) == 0)
        rv = fs_write_file(tmp, &next, sizeof(next));
    if (rv != sizeof(next))
        rv = rv < 0 ? rv : GZ_EIO;
    else
        rv = fs_rename(tmp, PREFS_DIR "/" DISPLAY_PREF);
    if (rv < 0) {
        sb_display_cancel(s);
        return rv;
    }
    s->saved = next;
    s->owner = 0;
    return 0;
}
