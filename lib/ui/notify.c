/* libui: icon badge and notifications through SpringBoard (glaze/sb.h). */

#include <glaze/notify.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <string.h>

static int request(uint32_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t *out)
{
    struct gz_msg m, r;

    memset(&m, 0, sizeof(m));
    m.w[0] = op;
    m.w[1] = a;
    m.w[2] = b;
    m.w[3] = (uint32_t)gz_getpid();
    m.w[4] = c;
    if (gz_send(ui.sb_port, &m, &r) < 0)
        return GZ_EIO;
    if (out)
        *out = r.w[1];
    return (int)r.w[0];
}

uint32_t ui_now_secs(void)
{
    struct gz_datetime dt;

    return gz_rtc_get(&dt) ? 0 : gz_dt_to_secs(&dt);
}

int ui_set_badge(int count)
{
    return request(SB_BADGE_SET, count < 0 ? 0 : (uint32_t)count, 0, 0, NULL);
}

int ui_notify_post(uint32_t id, uint32_t when, const char *title, const char *body, int badge)
{
    struct sb_note_req *q = &ui.shared->req;

    memset(q, 0, sizeof(*q));
    strlcpy(q->title, title ? title : "", sizeof(q->title));
    strlcpy(q->body, body ? body : "", sizeof(q->body));
    return request(SB_NOTE_POST, when, id, badge < 0 ? 0 : (uint32_t)badge, NULL);
}

int ui_notify_cancel(uint32_t id)
{
    return request(SB_NOTE_CANCEL, id, 0, 0, NULL);
}

uint32_t ui_notify_pending(uint32_t id)
{
    uint32_t when = 0;

    return request(SB_NOTE_PENDING, id, 0, 0, &when) ? 0 : when;
}
