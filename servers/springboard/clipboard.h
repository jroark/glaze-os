#ifndef SB_CLIPBOARD_H
#define SB_CLIPBOARD_H
#include <glaze/sb.h>

struct sb_clipboard {
    char text[SB_CLIPBOARD_MAX + 1], pending[SB_CLIPBOARD_MAX + 1];
    uint32_t length, expected, written, version, serial, transaction, deadline;
    int writer;
};

/* No pointers supplied by clients: data is the foreground app's existing
 * request area. Called only on the compositor thread. */
int sb_clipboard_request(struct sb_clipboard *c, const struct gz_msg *m,
                         struct gz_msg *r, void *data, int foreground_pid,
                         uint32_t now);
void sb_clipboard_cancel(struct sb_clipboard *c);
#endif
