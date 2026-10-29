/* Real client + server protocol, including torn transfers and editor actions. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <glaze/ui.h>
#include "../servers/springboard/clipboard.h"

struct ui_app ui;
static struct sb_shared shared[2];
static struct sb_clipboard clipboard;
static int pid = 7, front = 7, fail_write, change_read;
static uint32_t now;
int gz_getpid(void) { return pid; }
bool ui_in_background(void) { return ui.shared->background != 0; }
int gz_send(int port, const struct gz_msg *m, struct gz_msg *r)
{
    (void)port;
    memset(r, 0, sizeof(*r));
    if (fail_write && m->w[0] == SB_CLIPBOARD_WRITE && m->w[1]) return GZ_EIO;
    if (change_read && m->w[0] == SB_CLIPBOARD_READ && m->w[1]) {
        change_read = 0;
        struct gz_msg clear = {.w = {SB_CLIPBOARD_CLEAR,0,0,7}}, ignored = {0};
        assert(!sb_clipboard_request(&clipboard, &clear, &ignored, &ui.shared->req, 7, now));
    }
    r->w[0] = sb_clipboard_request(&clipboard, m, r, &ui.shared->req, front, now++);
    return 0;
}
static int raw(uint32_t op, uint32_t offset, uint32_t count, uint32_t token, struct gz_msg *r)
{
    struct gz_msg m = {.w = {op,offset,count,(uint32_t)pid,token}};
    memset(r, 0, sizeof(*r));
    return sb_clipboard_request(&clipboard, &m, r, &ui.shared->req, front, now++);
}
static void protocol(void)
{
    char original[UI_CLIPBOARD_MAX + 1], copy[sizeof(original)];
    for (int i = 0; i < UI_CLIPBOARD_MAX; i++) original[i] = 'A' + i % 26;
    original[UI_CLIPBOARD_MAX] = 0;
    assert(!ui_clipboard_set(original, UI_CLIPBOARD_MAX));
    pid = front = 8; ui.shared = &shared[1];
    assert(ui_clipboard_get(copy, sizeof(copy)) == UI_CLIPBOARD_MAX);
    assert(!memcmp(copy, original, sizeof(copy)));
    assert(ui_clipboard_get(copy, 8) == GZ_ENOSPC && !copy[0]);
    pid = front = 7; ui.shared = &shared[0];
    fail_write = 1;
    assert(ui_clipboard_set(original, 400) == GZ_EIO);
    fail_write = 0;
    assert(ui_clipboard_get(copy, sizeof(copy)) == UI_CLIPBOARD_MAX);
    assert(!memcmp(copy, original, sizeof(copy))); /* failed copy is atomic */
    struct gz_msg r;
    assert(!raw(SB_CLIPBOARD_BEGIN, 5, 0, 0, &r));
    uint32_t token = r.w[2];
    assert(raw(SB_CLIPBOARD_WRITE, UINT32_MAX, UINT32_MAX, token, &r) == GZ_EINVAL);
    assert(!clipboard.writer);
    assert(raw(SB_CLIPBOARD_COMMIT, 0, 0, token, &r) == GZ_EINVAL);
    assert(!raw(SB_CLIPBOARD_BEGIN, 5, 0, 0, &r)); token = r.w[2];
    memset(&ui.shared->req, 0, sizeof(ui.shared->req));
    assert(!raw(SB_CLIPBOARD_WRITE, 0, 5, token, &r));
    assert(raw(SB_CLIPBOARD_COMMIT, 0, 0, token, &r) == GZ_EINVAL);
    assert(!raw(SB_CLIPBOARD_BEGIN, 5, 0, 0, &r)); token = r.w[2];
    now += 3001;
    assert(raw(SB_CLIPBOARD_COMMIT, 0, 0, token, &r) == GZ_EINVAL);
    assert(!clipboard.writer);
    assert(!raw(SB_CLIPBOARD_BEGIN, 5, 0, 0, &r));
    front = 8;
    assert(raw(SB_CLIPBOARD_READ, 0, 0, 0, &r) == GZ_EPERM && !clipboard.writer);
    front = 7;
    ui.shared->background = 1;
    assert(ui_clipboard_get(copy, sizeof(copy)) == GZ_EPERM);
    ui.shared->background = 0;
    change_read = 1;
    assert(ui_clipboard_get(copy, sizeof(copy)) < 0 && !copy[0]);
    assert(ui_clipboard_get(copy, sizeof(copy)) == 0);
    assert(!ui_clipboard_set("kept", 4));
    assert(ui_clipboard_set("bad\0text", 8) == GZ_EINVAL);
    assert(ui_clipboard_get(copy, sizeof(copy)) == 4 && !strcmp(copy, "kept"));
    now = UINT32_MAX - 100;
    assert(!raw(SB_CLIPBOARD_BEGIN, 0, 0, 0, &r)); token = r.w[2];
    now = 100;
    assert(!raw(SB_CLIPBOARD_COMMIT, 0, 0, token, &r));
    assert(!ui_clipboard_clear());
    assert(!clipboard.length && !clipboard.writer);
    for (unsigned i = 0; i < sizeof(clipboard.text); i++) assert(!clipboard.text[i] && !clipboard.pending[i]);
}
static bool key(char *text, int cap, int *cursor, int code, unsigned flags)
{
    struct gz_input_event e = {.type = GZ_EV_KEY_DOWN, .code = code};
    return ui_edit_event(text, cap, cursor, &e, flags);
}
static void editor(void)
{
    char a[32] = "hello world", b[32] = "other"; int p = 11, q = 5;
    ui_edit_reset();
    assert(key(a, sizeof(a), &p, GZ_KEY_SELECT_ALL, 0));
    int start, end;
    assert(ui_edit_selection(a, p, &start, &end) && start == 0 && end == 11);
    assert(key(a, sizeof(a), &p, GZ_KEY_COPY, 0) && !ui_edit_modified());
    pid = front = 8; ui.shared = &shared[1]; ui_edit_reset(); /* another process */
    assert(key(b, sizeof(b), &q, GZ_KEY_SELECT_ALL, 0));
    assert(key(b, sizeof(b), &q, GZ_KEY_PASTE, 0) && ui_edit_modified());
    assert(!strcmp(a, b) && q == 11);
    assert(key(b, sizeof(b), &q, GZ_KEY_SELECT_LEFT, 0));
    assert(key(b, sizeof(b), &q, GZ_KEY_SELECT_LEFT, 0));
    assert(key(b, sizeof(b), &q, GZ_KEY_CUT, 0));
    assert(!strcmp(b, "hello wor") && q == 9);
    assert(ui_edit_key(b, sizeof(b), &q, UI_KEY_PASTE, 0));
    assert(!strcmp(b, "hello world"));
    assert(ui_edit_key(b, sizeof(b), &q, UI_KEY_SELECT_ALL, 0));
    assert(ui_edit_key(b, sizeof(b), &q, 'x', 0) && !strcmp(b, "x"));
    assert(key(b, sizeof(b), &q, GZ_KEY_SELECT_ALL, UI_EDIT_SECRET));
    assert(key(b, sizeof(b), &q, GZ_KEY_CUT, UI_EDIT_SECRET) && !strcmp(b, "x"));
    assert(key(b, sizeof(b), &q, GZ_KEY_COPY, UI_EDIT_SECRET));
    char copy[32]; assert(ui_clipboard_get(copy, sizeof(copy)) == 2 && !strcmp(copy, "ld"));
    assert(key(b, sizeof(b), &q, GZ_KEY_PASTE, UI_EDIT_SECRET) && !strcmp(b, "ld"));
    assert(!ui_clipboard_set("too long for field", 18));
    key(b, sizeof(b), &q, GZ_KEY_SELECT_ALL, 0);
    assert(ui_edit_key(b, 4, &q, UI_KEY_PASTE, 0) && !ui_edit_modified() && !strcmp(b, "ld"));
    assert(strstr(ui_edit_notice(), "fit"));
    assert(!ui_clipboard_set("12x", 3));
    assert(ui_edit_key(b, sizeof(b), &q, UI_KEY_PASTE, UI_EDIT_DIGITS) && !strcmp(b, "ld"));
    assert(!ui_clipboard_set("bad/name", 8));
    ui_edit_key(b, sizeof(b), &q, UI_KEY_PASTE, UI_EDIT_FILENAME);
    assert(!strcmp(b, "ld"));
    assert(!ui_clipboard_set("a\r\nb\tc", 6));
    ui_edit_key(b, sizeof(b), &q, UI_KEY_PASTE, 0); assert(!strcmp(b, "a b c"));
    ui_edit_key(b, sizeof(b), &q, UI_KEY_SELECT_ALL, 0);
    ui_edit_key(b, sizeof(b), &q, UI_KEY_PASTE, UI_EDIT_MULTILINE); assert(!strcmp(b, "a\nb c"));
    ui_edit_key(b, sizeof(b), &q, UI_KEY_SELECT_ALL, 0);
    assert(!ui_clipboard_clear());
    ui_edit_key(b, sizeof(b), &q, UI_KEY_PASTE, 0); assert(!strcmp(b, "a\nb c"));
    ui_edit_key(b, sizeof(b), &q, UI_KEY_LEFT, 0);
    ui_edit_key(b, sizeof(b), &q, UI_KEY_SELECT, 0);
    ui_edit_key(b, sizeof(b), &q, UI_KEY_RIGHT, 0);
    assert(ui_edit_selection(b, q, &start, &end) && start == 0 && end == 1);
    ui_edit_key(b, sizeof(b), &q, UI_KEY_COPY, 0);
    assert(ui_clipboard_get(copy, sizeof(copy)) == 1 && !strcmp(copy, "a"));
    ui_edit_set_cursor(b, &q, 5, false);
    assert(!ui_edit_selection(b, q, NULL, NULL));
    char latin[8] = {'c','a','f',(char)0xe9,0}; int pos = 4;
    key(latin, sizeof(latin), &pos, GZ_KEY_SELECT_LEFT, 0);
    key(latin, sizeof(latin), &pos, GZ_KEY_CUT, 0);
    assert(!strcmp(latin, "caf"));
    key(latin, sizeof(latin), &pos, GZ_KEY_PASTE, 0);
    assert((unsigned char)latin[3] == 0xe9 && latin[4] == 0);
    char large[UI_CLIPBOARD_MAX + 2]; memset(large, 'z', sizeof(large) - 1);large[sizeof(large) - 1] = 0;pos = 0;
    ui_edit_key(large, sizeof(large), &pos, UI_KEY_SELECT_ALL, 0);
    ui_edit_key(large, sizeof(large), &pos, UI_KEY_CUT, 0);
    assert(strlen(large) == sizeof(large) - 1); /* failed copy must never delete */
}
int main(void)
{
    ui.shared = &shared[0]; ui.sb_port = 1;
    protocol(); editor();
    puts("clipboard: protocol, cross-app transfers, atomicity, selection and field rules PASS");
}
