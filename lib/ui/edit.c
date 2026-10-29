/* One focused text editor per app; clipboard ownership belongs to SpringBoard. */
#include <glaze/ui.h>
#include <stdlib.h>
#include <string.h>

static struct {
    char *text;
    int *cursor, anchor, last_cursor, length;
    bool selecting, modified;
    const char *notice;
} edit;

void ui_edit_reset(void) { memset(&edit, 0, sizeof(edit)); }
bool ui_edit_modified(void) { return edit.modified; }
bool ui_edit_selecting(void) { return edit.selecting; }
const char *ui_edit_notice(void)
{
    return edit.notice ? edit.notice : edit.selecting ? "Select with the arrows" : "Choose an editing command";
}

static int boundary(const char *text, int n, int p)
{
    (void)text;
    return MAX(0, MIN(n, p)); /* libgfx and native editors use 8-bit glyphs. */
}
static int previous(const char *text, int p) { return boundary(text, p, p - 1); }
static int next(const char *text, int n, int p)
{
    (void)text;
    return MIN(n, p + 1);
}
static void bind(char *text, int *cursor)
{
    int n = (int)strlen(text), p = boundary(text, n, *cursor);
    if (edit.text != text || edit.cursor != cursor || edit.last_cursor != p || edit.length != n) {
        ui_edit_reset();
        edit.text = text; edit.cursor = cursor; edit.anchor = p;
    }
    *cursor = edit.last_cursor = p;
    edit.length = n;
    edit.modified = false;
}
static void sync(void)
{
    edit.last_cursor = *edit.cursor;
    edit.length = (int)strlen(edit.text);
}
bool ui_edit_selection(const char *text, int cursor, int *start, int *end)
{
    if (edit.text != text || edit.last_cursor != cursor || edit.length != (int)strlen(text)) return false;
    int a = MIN(edit.anchor, cursor), b = MAX(edit.anchor, cursor);
    if (start) *start = a;
    if (end) *end = b;
    return a != b;
}
void ui_edit_set_cursor(char *text, int *cursor, int position, bool extend)
{
    bind(text, cursor);
    *cursor = boundary(text, edit.length, position);
    if (!extend) { edit.anchor = *cursor; edit.selecting = false; }
    edit.notice = NULL;
    sync();
}

static bool error(int code)
{
    edit.notice = code == GZ_ENOSPC ? "Text will not fit" : code == GZ_EINVAL ? "Text not allowed here" : "Clipboard unavailable";
    return true;
}
static void replace(int start, int end, const char *text, int length)
{
    int n = edit.length;
    if (start != end || length) {
        memmove(edit.text + start + length, edit.text + end, (size_t)(n - end + 1));
        if (length) memcpy(edit.text + start, text, length);
        edit.modified = true;
    }
    *edit.cursor = edit.anchor = start + length;
    edit.selecting = false;
    sync();
}
static bool allowed(int ch, unsigned flags)
{
    if (flags & UI_EDIT_DIGITS) return ch >= '0' && ch <= '9';
    if (flags & UI_EDIT_URL) return ch > ' ' && ch != 127;
    if (flags & UI_EDIT_FILENAME) return ch >= ' ' && ch != 127 && ch != '/' && ch != '\\';
    return ch >= ' ' || (ch == '\n' && (flags & UI_EDIT_MULTILINE));
}

bool ui_edit_key(char *text, int cap, int *cursor, int key, unsigned flags)
{
    edit.modified = false;
    if (!text || !cursor || cap < 1 || !memchr(text, 0, cap)) return false;
    bind(text, cursor);
    int a = MIN(edit.anchor, *cursor), b = MAX(edit.anchor, *cursor), n = edit.length;
    edit.notice = NULL;
    switch (key) {
    case UI_KEY_SELECT_ALL:
        edit.anchor = 0; *cursor = n; edit.selecting = false; sync(); return true;
    case UI_KEY_SELECT:
        edit.selecting = !edit.selecting;
        if (edit.selecting) edit.anchor = *cursor;
        return true;
    case UI_KEY_LEFT:
    case UI_KEY_RIGHT:
        if (!edit.selecting && a != b) *cursor = key == UI_KEY_LEFT ? a : b;
        else *cursor = key == UI_KEY_LEFT ? previous(text, *cursor) : next(text, n, *cursor);
        if (!edit.selecting) edit.anchor = *cursor;
        sync(); return true;
    case UI_KEY_CLEAR_CLIP: {
        int rc = ui_clipboard_clear();
        if (rc) return error(rc);
        edit.notice = "Clipboard cleared"; return true;
    }
    case UI_KEY_COPY:
    case UI_KEY_CUT: {
        if ((flags & UI_EDIT_SECRET) || (key == UI_KEY_CUT && (flags & UI_EDIT_READONLY))) {
            edit.notice = "Copy or cut not allowed"; return true;
        }
        if (a == b) { edit.notice = "Select some text first"; return true; }
        int rc = ui_clipboard_set(text + a, b - a);
        if (rc) return error(rc);
        if (key == UI_KEY_CUT) replace(a, b, NULL, 0);
        edit.notice = key == UI_KEY_CUT ? "Cut" : "Copied";
        return true;
    }
    case UI_KEY_PASTE: {
        if (flags & UI_EDIT_READONLY) return true;
        char *copy = malloc(UI_CLIPBOARD_MAX + 1);
        if (!copy) return error(GZ_ENOMEM);
        int rc = ui_clipboard_get(copy, UI_CLIPBOARD_MAX + 1), length = 0;
        if (rc >= 0) {
            for (int i = 0; i < rc; i++) {
                unsigned char ch = (unsigned char)copy[i];
                if (ch == '\r') { if (i + 1 < rc && copy[i + 1] == '\n') i++; ch = '\n'; }
                if (ch == '\t' || (ch == '\n' && !(flags & UI_EDIT_MULTILINE))) ch = ' ';
                if (!allowed(ch, flags)) { rc = GZ_EINVAL; break; }
                copy[length++] = ch;
            }
            if (rc >= 0 && length > cap - 1 - (n - (b - a))) rc = GZ_ENOSPC;
            if (rc >= 0 && length) replace(a, b, copy, length);
        }
        memset(copy, 0, UI_CLIPBOARD_MAX + 1);
        free(copy);
        if (rc < 0) return error(rc);
        edit.notice = length ? "Pasted" : "Clipboard is empty";
        return true;
    }
    case UI_KEY_DOTCOM:
        if (flags & (UI_EDIT_READONLY | UI_EDIT_DIGITS)) return true;
        if (4 > cap - 1 - (n - (b - a))) return error(GZ_ENOSPC);
        replace(a, b, ".com", 4); return true;
    case '\b':
        if (flags & UI_EDIT_READONLY) return true;
        if (a == b) a = previous(text, a);
        replace(a, b, NULL, 0); return true;
    default:
        if (key < 32 && key != '\n') return false;
        if (key > 126 || !allowed(key, flags)) return false;
        if (flags & UI_EDIT_READONLY) return true;
        if (n - (b - a) + 1 >= cap) return error(GZ_ENOSPC);
        char ch = (char)key;
        replace(a, b, &ch, 1); return true;
    }
}

bool ui_edit_event(char *text, int cap, int *cursor, const struct gz_input_event *e, unsigned flags)
{
    edit.modified = false;
    if (e->type == GZ_EV_TEXT) return ui_edit_key(text, cap, cursor, e->code, flags);
    if (e->type != GZ_EV_KEY_DOWN) return false;
    int command = 0;
    switch (e->code) {
    case GZ_KEY_COPY: command = UI_KEY_COPY; break;
    case GZ_KEY_CUT: command = UI_KEY_CUT; break;
    case GZ_KEY_PASTE: command = UI_KEY_PASTE; break;
    case GZ_KEY_SELECT_ALL: command = UI_KEY_SELECT_ALL; break;
    case GZ_KEY_LEFT: command = UI_KEY_LEFT; break;
    case GZ_KEY_RIGHT: command = UI_KEY_RIGHT; break;
    case GZ_KEY_BACKSPACE: command = '\b'; break;
    case GZ_KEY_ENTER: command = '\n'; break;
    }
    if (command) return ui_edit_key(text, cap, cursor, command, flags);
    if (!text || !cursor || cap < 1 || !memchr(text, 0, cap)) return false;
    bind(text, cursor);
    int p = *cursor, n = edit.length;
    bool extend = e->code == GZ_KEY_SELECT_LEFT || e->code == GZ_KEY_SELECT_RIGHT ||
                  e->code == GZ_KEY_SELECT_HOME || e->code == GZ_KEY_SELECT_END;
    switch (e->code) {
    case GZ_KEY_SELECT_LEFT: p = previous(text, p); break;
    case GZ_KEY_SELECT_RIGHT: p = next(text, n, p); break;
    case GZ_KEY_DELETE:
        if (flags & UI_EDIT_READONLY) return true;
        replace(MIN(edit.anchor, p), edit.anchor != p ? MAX(edit.anchor, p) : next(text, n, p), NULL, 0);
        return true;
    case GZ_KEY_UP:
    case GZ_KEY_DOWN: {
        if (!(flags & UI_EDIT_MULTILINE)) return false;
        int start = p, end;
        while (start && text[start - 1] != '\n') start--;
        int column = p - start;
        if (e->code == GZ_KEY_UP) {
            if (!start) break;
            end = start - 1; start = end;
            while (start && text[start - 1] != '\n') start--;
        } else {
            while (start < n && text[start] != '\n') start++;
            if (start == n) break;
            end = ++start;
            while (end < n && text[end] != '\n') end++;
        }
        p = boundary(text, n, MIN(start + column, end)); break;
    }
    case GZ_KEY_KBD_HOME:
    case GZ_KEY_SELECT_HOME:
        if (flags & UI_EDIT_MULTILINE) { while (p && text[p - 1] != '\n') p--; }
        else p = 0;
        break;
    case GZ_KEY_END:
    case GZ_KEY_SELECT_END:
        if (flags & UI_EDIT_MULTILINE) { while (p < n && text[p] != '\n') p++; }
        else p = n;
        break;
    default: return false;
    }
    *cursor = p;
    if (!extend) { edit.anchor = p; edit.selecting = false; }
    edit.notice = NULL;
    sync(); return true;
}
