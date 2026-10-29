/* Clipboard substitute for native UI previews. Device builds use the IPC
 * client; tests/clipboard.c exercises that client and the actual server. */
#include <glaze/ui.h>
#include <string.h>
static char contents[UI_CLIPBOARD_MAX + 1];
int ui_clipboard_set(const char *text, int length)
{
    if (!text || length < 0) return GZ_EINVAL;
    if (length > UI_CLIPBOARD_MAX) return GZ_ENOSPC;
    if (memchr(text, 0, length)) return GZ_EINVAL;
    memcpy(contents, text, length); contents[length] = 0; return 0;
}
int ui_clipboard_get(char *text, int capacity)
{
    int length = strlen(contents);
    if (!text || capacity < 1) return GZ_EINVAL;
    text[0] = 0;
    if (length >= capacity) return GZ_ENOSPC;
    memcpy(text, contents, length + 1); return length;
}
int ui_clipboard_clear(void) { memset(contents, 0, sizeof(contents)); return 0; }
