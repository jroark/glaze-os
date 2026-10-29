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
    return ui_edit_event(text, cap, cursor, e, multiline ? UI_EDIT_MULTILINE : 0);
}
