/* The shared clipboard lives in SpringBoard, never in a preference/file. */
#include <glaze/ui.h>
#include <glaze/sys.h>
#include <string.h>

static int request(uint32_t op, uint32_t offset, uint32_t count,
                   uint32_t version, struct gz_msg *r)
{
    if (!ui.shared || ui.sb_port < 0 || ui_in_background()) return GZ_EPERM;
    struct gz_msg m = {0};
    m.w[0] = op; m.w[1] = offset; m.w[2] = count;
    m.w[3] = gz_getpid(); m.w[4] = version;
    int rc = gz_send(ui.sb_port, &m, r);
    return rc < 0 ? rc : (int)r->w[0];
}

int ui_clipboard_set(const char *text, int length)
{
    if (!text || length < 0) return GZ_EINVAL;
    if (length > UI_CLIPBOARD_MAX) return GZ_ENOSPC;
    if (memchr(text, 0, length)) return GZ_EINVAL;
    struct gz_msg r;
    int rc = request(SB_CLIPBOARD_BEGIN, length, 0, 0, &r);
    if (rc) return rc;
    uint32_t transaction = r.w[2];
    for (int offset = 0; offset < length;) {
        int count = MIN(length - offset, (int)sizeof(ui.shared->req));
        memcpy(&ui.shared->req, text + offset, count);
        rc = request(SB_CLIPBOARD_WRITE, offset, count, transaction, &r);
        if (rc) return rc;
        offset += count;
    }
    return request(SB_CLIPBOARD_COMMIT, 0, 0, transaction, &r);
}

int ui_clipboard_get(char *text, int capacity)
{
    if (!text || capacity < 1) return GZ_EINVAL;
    text[0] = 0;
    struct gz_msg r;
    int rc = request(SB_CLIPBOARD_READ, 0, 0, 0, &r);
    if (rc) return rc;
    uint32_t length = r.w[1], version = r.w[2];
    if (length > UI_CLIPBOARD_MAX || length >= (uint32_t)capacity) return GZ_ENOSPC;
    for (uint32_t offset = 0; offset < length;) {
        uint32_t count = MIN(length - offset, sizeof(ui.shared->req));
        rc = request(SB_CLIPBOARD_READ, offset, count, version, &r);
        if (rc || r.w[1] != length || r.w[2] != version || r.w[4] != count) {
            text[0] = 0;
            return rc ? rc : GZ_EIO;
        }
        memcpy(text + offset, &ui.shared->req, count);
        offset += count;
    }
    text[length] = 0;
    return (int)length;
}

int ui_clipboard_clear(void)
{
    struct gz_msg r;
    return request(SB_CLIPBOARD_CLEAR, 0, 0, 0, &r);
}
