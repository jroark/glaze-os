#include "clipboard.h"
#include <string.h>

void sb_clipboard_cancel(struct sb_clipboard *c)
{
    memset(c->pending, 0, sizeof(c->pending));
    c->writer = 0;
    c->expected = c->written = c->transaction = 0;
}

static uint32_t next_serial(struct sb_clipboard *c)
{
    if (!++c->serial) ++c->serial;
    return c->serial;
}

int sb_clipboard_request(struct sb_clipboard *c, const struct gz_msg *m,
                         struct gz_msg *r, void *data, int foreground_pid,
                         uint32_t now)
{
    uint32_t op = m->w[0], offset = m->w[1], count = m->w[2];
    int pid = (int)m->w[3];
    if (c->writer && (c->writer != foreground_pid || (int32_t)(now - c->deadline) >= 0))
        sb_clipboard_cancel(c);
    if (!data || pid <= 0 || pid != foreground_pid) return GZ_EPERM;
    switch (op) {
    case SB_CLIPBOARD_CLEAR:
        memset(c->text, 0, sizeof(c->text));
        c->length = 0;
        c->version = next_serial(c);
        sb_clipboard_cancel(c);
        return 0;
    case SB_CLIPBOARD_BEGIN:
        if (offset > SB_CLIPBOARD_MAX) return GZ_ENOSPC;
        sb_clipboard_cancel(c);
        c->expected = offset;
        c->writer = pid;
        c->transaction = next_serial(c);
        c->deadline = now + 3000;
        r->w[2] = c->transaction;
        return 0;
    case SB_CLIPBOARD_WRITE:
    case SB_CLIPBOARD_COMMIT:
        if (c->writer != pid || !c->transaction || m->w[4] != c->transaction)
            return GZ_EINVAL;
        if (op == SB_CLIPBOARD_WRITE) {
            if (count > sizeof(struct sb_note_req) || offset != c->written ||
                count > c->expected - c->written) {
                sb_clipboard_cancel(c);
                return GZ_EINVAL;
            }
            memcpy(c->pending + c->written, data, count);
            c->written += count;
            c->deadline = now + 3000;
            return 0;
        }
        if (c->written != c->expected || memchr(c->pending, 0, c->written)) {
            sb_clipboard_cancel(c);
            return GZ_EINVAL;
        }
        memset(c->text, 0, sizeof(c->text));
        memcpy(c->text, c->pending, c->written);
        c->length = c->written;
        c->version = next_serial(c);
        sb_clipboard_cancel(c);
        return 0;
    case SB_CLIPBOARD_READ:
        if (count > sizeof(struct sb_note_req) || offset > c->length) return GZ_EINVAL;
        if (count && m->w[4] != c->version) return GZ_EBUSY;
        count = MIN(count, c->length - offset);
        memset(data, 0, sizeof(struct sb_note_req));
        memcpy(data, c->text + offset, count);
        r->w[1] = c->length;
        r->w[2] = c->version;
        r->w[4] = count;
        return 0;
    }
    return GZ_EINVAL;
}
