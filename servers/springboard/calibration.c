#include "calibration.h"
#include <glaze/fs.h>
#include <glaze/prefs.h>
#include <glaze/sb.h>

void sb_calibration_cancel(struct sb_calibration *s)
{
    s->owner = 0;
    s->preview = false;
}

void sb_calibration_load(struct sb_calibration *s)
{
    /* Read one extra byte to reject records with an unexpected length. */
    struct { struct touch_calibration c; uint32_t extra; } record;
    touch_identity(&s->saved);
    if (pref_get(TOUCH_PREF, &record, sizeof(record)) == sizeof(record.c) && touch_valid(&record.c))
        s->saved = record.c;
    sb_calibration_cancel(s);
}

bool sb_calibration_expire(struct sb_calibration *s, uint32_t now)
{
    if (!s->owner || !s->preview || (int32_t)(now - s->until) < 0)
        return false;
    sb_calibration_cancel(s);
    return true;
}

int sb_calibration_request(struct sb_calibration *s, uint32_t op, int pid,
                           const struct touch_calibration *candidate, uint32_t now)
{
    sb_calibration_expire(s, now);
    switch (op) {
    case SB_TOUCH_BEGIN:
        s->owner = pid;
        s->preview = false;
        return 0;
    case SB_TOUCH_CANCEL:
        if (s->owner && s->owner != pid)
            return GZ_EPERM;
        sb_calibration_cancel(s);
        return 0;
    case SB_TOUCH_RESET: {
        int rv = pref_delete(TOUCH_PREF);
        if (rv < 0 && rv != GZ_ENOENT)
            return rv;
        touch_identity(&s->saved);
        sb_calibration_cancel(s);
        return 0;
    }
    case SB_TOUCH_PREVIEW:
        if (s->owner != pid || !candidate || !touch_valid(candidate))
            return GZ_EINVAL;
        s->candidate = *candidate;
        s->preview = true;
        s->until = now + TOUCH_PREVIEW_MS;
        return 0;
    case SB_TOUCH_SAVE: {
        if (s->owner != pid || !s->preview)
            return GZ_EINVAL;
        /* Commit through a temporary preference so an I/O failure cannot
         * truncate the previous calibration. littlefs rename is atomic. */
        const char *tmp = PREFS_DIR "/" TOUCH_PREF ".tmp";
        int rv = fs_write_file(tmp, &s->candidate, sizeof(s->candidate));
        if (rv == GZ_ENOENT && fs_mkdirs(PREFS_DIR) == 0)
            rv = fs_write_file(tmp, &s->candidate, sizeof(s->candidate));
        if (rv != sizeof(s->candidate))
            return rv < 0 ? rv : GZ_EIO;
        rv = fs_rename(tmp, PREFS_DIR "/" TOUCH_PREF);
        if (rv < 0)
            return rv;
        s->saved = s->candidate;
        sb_calibration_cancel(s);
        return 0;
    }
    }
    return GZ_EINVAL;
}

bool sb_calibration_input(const struct sb_calibration *s, struct gz_input_event *ev)
{
    if (ev->type != GZ_EV_TOUCH_DOWN && ev->type != GZ_EV_TOUCH_MOVE && ev->type != GZ_EV_TOUCH_UP)
        return false;
    if (s->owner && !s->preview)
        return true;
    struct touch_point p;
    touch_apply(s->owner && s->preview ? &s->candidate : &s->saved, ev->x, ev->y, &p);
    ev->x = (int16_t)MAX(0, MIN(GZ_FB_WIDTH - 1, p.x));
    ev->y = (int16_t)MAX(0, MIN(GZ_FB_HEIGHT - 1, p.y));
    return false;
}
