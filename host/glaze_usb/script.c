#include "script.h"
#include <errno.h>
#include <glaze/abi.h>
#include <glaze/usb_wire.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char *space(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}
static int number(const char **p, long *v)
{
    char *end;
    errno = 0;
    *p = space(*p);
    *v = strtol(*p, &end, 10);
    if (end == *p || errno || (*end && *end != ' ' && *end != '\t' && *end != '#'))
        return -1;
    *p = end;
    return 0;
}
static int add(uint8_t *out, unsigned *n, uint32_t ms, unsigned type, unsigned code, int x, int y)
{
    if (*n == GU_SCRIPT_MAX || ms > GU_SCRIPT_MS || x < INT16_MIN || x > INT16_MAX || y < INT16_MIN ||
        y > INT16_MAX)
        return -1;
    /* Stable insertion preserves line order for simultaneous events. */
    unsigned at = *n;
    while (at && gu_get32(out + (at - 1) * 12) > ms)
        at--;
    memmove(out + (at + 1) * 12, out + at * 12, (*n - at) * 12);
    uint8_t *b = out + at * 12;
    gu_put32(b, ms);
    b[4] = type;
    b[5] = type >> 8;
    b[6] = code;
    b[7] = code >> 8;
    b[8] = x;
    b[9] = (uint16_t)x >> 8;
    b[10] = y;
    b[11] = (uint16_t)y >> 8;
    (*n)++;
    return 0;
}
int usb_script_compile(const char *text, uint8_t *out, unsigned *n, unsigned *line)
{
    static const struct {
        const char *name;
        unsigned code;
    } keys[] = {{"ok", GZ_KEY_OK},
                {"esc", GZ_KEY_ESC},
                {"up", GZ_KEY_UP},
                {"down", GZ_KEY_DOWN},
                {"left", GZ_KEY_LEFT},
                {"right", GZ_KEY_RIGHT},
                {"home", GZ_KEY_HOME},
                {"power", GZ_KEY_POWER},
                {"enter", GZ_KEY_ENTER},
                {"tab", GZ_KEY_TAB},
                {"backspace", GZ_KEY_BACKSPACE},
                {"delete", GZ_KEY_DELETE},
                {"end", GZ_KEY_END},
                {"pageup", GZ_KEY_PAGEUP},
                {"pagedown", GZ_KEY_PAGEDOWN},
                {"selectall", GZ_KEY_SELECT_ALL}, {"copy", GZ_KEY_COPY},
                {"cut", GZ_KEY_CUT}, {"paste", GZ_KEY_PASTE},
                {"selectleft", GZ_KEY_SELECT_LEFT}, {"selectright", GZ_KEY_SELECT_RIGHT},
                {"selecthome", GZ_KEY_SELECT_HOME}, {"selectend", GZ_KEY_SELECT_END}};
    *n = 0;
    *line = 0;
    while (*text) {
        (*line)++;
        size_t len = strcspn(text, "\r\n");
        if (len >= 4096)
            return -1;
        char buffer[4096];
        memcpy(buffer, text, len);
        buffer[len] = 0;
        text += len;
        if (*text == '\r')
            text++;
        if (*text == '\n')
            text++;
        const char *p = space(buffer);
        if (!*p || *p == '#')
            continue;
        long t;
        if (number(&p, &t) || t < 0 || t > GU_SCRIPT_MS)
            return -1;
        p = space(p);
        const char *word = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        size_t w = (size_t)(p - word);
#define IS(s) (w == sizeof(s) - 1 && !memcmp(word, s, w))
#define ADD(tm, ty, co, xx, yy)                                                                              \
    do {                                                                                                     \
        if (add(out, n, (uint32_t)(tm), ty, co, xx, yy))                                                     \
            return -1;                                                                                       \
    } while (0)
        if (IS("text")) {
            if (*p != ' ' && *p != '\t')
                return -1;
            for (p++; *p; p++, t += 40) {
                if ((unsigned char)*p > 127)
                    return -1;
                ADD(t, GZ_EV_TEXT, (unsigned char)*p, 0, 0);
            }
            continue;
        } else if (IS("key")) {
            p = space(p);
            unsigned code = 0;
            for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
                size_t k = strlen(keys[i].name);
                if (!strncmp(p, keys[i].name, k) && (!p[k] || p[k] == ' ' || p[k] == '\t' || p[k] == '#')) {
                    code = keys[i].code;
                    p += k;
                    break;
                }
            }
            if (!code)
                return -1;
            ADD(t, GZ_EV_KEY_DOWN, code, 0, 0);
            ADD(t + 60, GZ_EV_KEY_UP, code, 0, 0);
        } else {
            long a[5];
            unsigned argc = IS("drag") ? 5 : 2;
            if (!IS("drag") && !IS("tap") && !IS("down") && !IS("move") && !IS("up"))
                return -1;
            for (unsigned i = 0; i < argc; i++)
                if (number(&p, a + i))
                    return -1;
            for (unsigned i = 0; i < (argc == 5 ? 4u : 2u); i++)
                if (a[i] < INT16_MIN || a[i] > INT16_MAX)
                    return -1;
            if (IS("drag")) {
                if (a[4] < 0 || a[4] > GU_SCRIPT_MS || t + a[4] + 20 > GU_SCRIPT_MS)
                    return -1;
                long steps = a[4] / 30 + 1;
                if (steps + 2 > (long)(GU_SCRIPT_MAX - *n))
                    return -1;
                ADD(t, GZ_EV_TOUCH_DOWN, 0, a[0], a[1]);
                for (long i = 1; i <= steps; i++)
                    ADD(t + a[4] * i / steps, GZ_EV_TOUCH_MOVE, 0, a[0] + (a[2] - a[0]) * i / steps,
                        a[1] + (a[3] - a[1]) * i / steps);
                ADD(t + a[4] + 20, GZ_EV_TOUCH_UP, 0, a[2], a[3]);
            } else if (IS("tap")) {
                ADD(t, GZ_EV_TOUCH_DOWN, 0, a[0], a[1]);
                ADD(t + 80, GZ_EV_TOUCH_UP, 0, a[0], a[1]);
            } else
                ADD(t,
                    IS("down")   ? GZ_EV_TOUCH_DOWN
                    : IS("move") ? GZ_EV_TOUCH_MOVE
                                 : GZ_EV_TOUCH_UP,
                    0, a[0], a[1]);
        }
        p = space(p);
        if (*p && *p != '#')
            return -1;
#undef IS
#undef ADD
    }
    return 0;
}
