/* Word wrapping and text hit-testing. */

#include <glaze/ui.h>
#include <string.h>

void ui_draw_edit_selection(gfx_surface *s, const gfx_font *font, const char *text,
                            int cursor, int start, int length, int x, int y, int height)
{
    int a, b;
    if (!ui_edit_selection(text, cursor, &a, &b)) return;
    a = MAX(a, start); b = MIN(b, start + length);
    if (a >= b) return;
    int left = gfx_text_width_n(font, text + start, a - start);
    int right = gfx_text_width_n(font, text + start, b - start);
    gfx_fill(s, gfx_r(x + left, y, MAX(2, right - left), height), 0xB6D8FF);
}

int ui_wrap(const gfx_font *f, const char *text, int width, int *starts, int max_lines)
{
    int n = 0, pos = 0, len = (int)strlen(text);

    if (max_lines <= 0)
        return 0;
    starts[n++] = 0;
    while (pos < len && n < max_lines) {
        int line = pos, last_break = -1, w = 0, i = pos;
        for (; i < len && text[i] != '\n'; i++) {
            w += gfx_text_width_n(f, text + i, 1);
            if (text[i] == ' ')
                last_break = i;
            if (w > width && i > line) {
                if (last_break > line)
                    i = last_break + 1;     /* wrap after the space */
                break;
            }
        }
        if (i < len && text[i] == '\n')
            i++;
        else if (i >= len)
            break;
        starts[n++] = i;
        pos = i;
    }
    return n;
}

int ui_line_len(const char *text, const int *starts, int n, int line)
{
    int end = line + 1 < n ? starts[line + 1] : (int)strlen(text);
    int len = end - starts[line];

    while (len > 0 && (text[starts[line] + len - 1] == '\n'))
        len--;
    return len;
}

int ui_text_hit(const gfx_font *f, const char *text, const int *starts, int n, int line, int x)
{
    if (line < 0)
        return 0;
    if (line >= n)
        return (int)strlen(text);
    int len = ui_line_len(text, starts, n, line);
    for (int i = 0; i < len; i++) {
        int a = gfx_text_width_n(f, text + starts[line], i);
        int b = gfx_text_width_n(f, text + starts[line], i + 1);
        if (x < (a + b) / 2)
            return starts[line] + i;
    }
    return starts[line] + len;
}
