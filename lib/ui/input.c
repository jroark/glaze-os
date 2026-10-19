/* Physical keyboard helpers. Editing focus belongs to the app. */
#include <glaze/ui.h>
#include <string.h>

bool ui_has_hardware_keyboard(void) { return ui.hardware_keyboard; }
int ui_keyboard_height(void) { return ui_has_hardware_keyboard() ? 0 : UI_KB_H; }
int ui_input_char(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_TEXT)
        return e->code >= ' ' && e->code < 0x7f ? e->code : 0;
    if (e->type == GZ_EV_KEY_DOWN) {
        if (e->code == GZ_KEY_ENTER) return '\n';
        if (e->code == GZ_KEY_BACKSPACE) return '\b';
    }
    return 0;
}

bool ui_edit_input(char *text, int cap, int *cursor, const struct gz_input_event *e, bool multiline)
{
    int n = (int)strlen(text), p = *cursor, ch = ui_input_char(e);
    if (p < 0) p = 0;
    if (p > n) p = n;
    if (ch == '\b') {
        if (p) { memmove(text + p - 1, text + p, (size_t)(n - p + 1)); p--; }
    } else if (ch >= ' ' || (ch == '\n' && multiline)) {
        if (n + 1 < cap) {
            memmove(text + p + 1, text + p, (size_t)(n - p + 1));
            text[p++] = (char)ch;
        }
    } else if (e->type == GZ_EV_KEY_DOWN) {
        switch (e->code) {
        case GZ_KEY_DELETE:
            if (p < n) memmove(text + p, text + p + 1, (size_t)(n - p));
            break;
        case GZ_KEY_LEFT: if (p) p--; break;
        case GZ_KEY_RIGHT: if (p < n) p++; break;
        case GZ_KEY_UP:
        case GZ_KEY_DOWN: {
            if (!multiline) return false;
            int start = p;
            while (start && text[start - 1] != '\n') start--;
            int col = p - start, end;
            if (e->code == GZ_KEY_UP) {
                if (!start) break;
                end = start - 1;
                start = end;
                while (start && text[start - 1] != '\n') start--;
            } else {
                while (start < n && text[start] != '\n') start++;
                if (start == n) break;
                start++;
                end = start;
                while (end < n && text[end] != '\n') end++;
            }
            p = MIN(start + col, end);
            break;
        }
        case GZ_KEY_KBD_HOME:
            if (multiline) { while (p && text[p - 1] != '\n') p--; }
            else p = 0;
            break;
        case GZ_KEY_END:
            if (multiline) { while (p < n && text[p] != '\n') p++; }
            else p = n;
            break;
        default: return false;
        }
    } else return false;
    *cursor = p;
    return true;
}
