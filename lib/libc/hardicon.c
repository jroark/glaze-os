#include <glaze/hardicon.h>
#include <glaze/prefs.h>
#include <stdio.h>
#include <string.h>

/*
 * Windows CE's touch.dll keeps the strip as seven records { id, 3, x_lo,
 * x_hi, VK_F1 + id - 1 } in its calibrated quarter-pixel units, after its
 * "HARDWARE\DEVICEMAP\TOUCH\HardIcon" string (stock NK, flat offset
 * 0x4d108c). These are the x_hi of each, 0..959 being the 240 columns.
 */
static const uint16_t zone_end[HARDICON_COUNT] = { 137, 274, 411, 548, 685, 822, 959 };

/* COShell's SOFTWARE\CASIO\COShell\HardIcon\00..06 "File" values, in order. */
static const char *const labels[HARDICON_COUNT] = {
    "Calendar", "Contacts", "Tasks", "Notes", "Mail", "Internet", "Control Panel",
};

const uint16_t hardicon_keys[] = {
    GZ_KEY_ESC, GZ_KEY_OK, GZ_KEY_ENTER, GZ_KEY_UP, GZ_KEY_DOWN, GZ_KEY_LEFT, GZ_KEY_RIGHT,
    GZ_KEY_PAGEUP, GZ_KEY_PAGEDOWN, GZ_KEY_TAB, GZ_KEY_BACKSPACE,
};
const int hardicon_num_keys = (int)(sizeof(hardicon_keys) / sizeof(hardicon_keys[0]));
static const char *const key_names[] = {
    "esc", "ok", "enter", "up", "down", "left", "right", "pageup", "pagedown", "tab", "backspace",
};

static const char *const kind_names[HARDICON_KINDS] = {
    "none", "home", "switcher", "lock", "sleep", "screenoff", "app", "pkg", "key",
};

int hardicon_zone(int x)
{
    int q = x * 4 + 2;      /* the column's centre */
    for (int i = 0; i < HARDICON_COUNT; i++)
        if (q <= zone_end[i])
            return i;
    return HARDICON_COUNT - 1;
}

const char *hardicon_label(int n)
{
    return n >= 0 && n < HARDICON_COUNT ? labels[n] : "";
}

static void set_target(struct hardicon_action *a, enum hardicon_kind kind, const char *target)
{
    *a = (struct hardicon_action){ .kind = (uint8_t)kind };
    strlcpy(a->target, target, sizeof(a->target));
}

/* Glaze has no Contacts or Tasks; Mail is the Gmail package when installed. */
void hardicon_defaults(struct hardicon_config *c)
{
    memset(c, 0, sizeof(*c));
    set_target(&c->icon[0], HARDICON_APP, "bin/calendar");
    set_target(&c->icon[3], HARDICON_APP, "bin/notes");
    set_target(&c->icon[4], HARDICON_PACKAGE, "gmail");
    set_target(&c->icon[5], HARDICON_APP, "bin/browser");
    set_target(&c->icon[6], HARDICON_APP, "bin/settings");
    c->apps = true;
}

const char *hardicon_key_name(uint16_t key)
{
    for (int i = 0; i < hardicon_num_keys; i++)
        if (hardicon_keys[i] == key)
            return key_names[i];
    return NULL;
}

uint16_t hardicon_key_code(const char *name, int len)
{
    for (int i = 0; i < hardicon_num_keys; i++)
        if ((int)strlen(key_names[i]) == len && !strncmp(key_names[i], name, (size_t)len))
            return hardicon_keys[i];
    return 0;
}

int hardicon_format(const struct hardicon_config *c, char *buf, int len)
{
    int n = 0;
    for (int i = 0; i < HARDICON_COUNT; i++) {
        const struct hardicon_action *a = &c->icon[i];
        const char *kind = a->kind < HARDICON_KINDS ? kind_names[a->kind] : "none";
        const char *arg = a->kind == HARDICON_APP || a->kind == HARDICON_PACKAGE ? a->target
                        : a->kind == HARDICON_KEY ? hardicon_key_name(a->key) : NULL;
        n += snprintf(buf + n, n < len ? (size_t)(len - n) : 0, arg ? "%s %s\n" : "%s\n", kind, arg);
    }
    n += snprintf(buf + n, n < len ? (size_t)(len - n) : 0, "apps %d\n", c->apps ? 1 : 0);
    return n;
}

static bool parse_line(struct hardicon_action *a, const char *p, int len)
{
    int w = 0;
    while (w < len && p[w] != ' ')
        w++;
    const char *arg = w < len ? p + w + 1 : p + len;
    int arg_len = (int)(p + len - arg);
    for (int k = 0; k < HARDICON_KINDS; k++) {
        if ((int)strlen(kind_names[k]) != w || strncmp(kind_names[k], p, (size_t)w))
            continue;
        struct hardicon_action v = { .kind = (uint8_t)k };
        if (k == HARDICON_APP || k == HARDICON_PACKAGE) {
            if (arg_len <= 0 || arg_len >= HARDICON_TARGET || memchr(arg, ' ', (size_t)arg_len))
                return false;
            memcpy(v.target, arg, (size_t)arg_len);
        } else if (k == HARDICON_KEY) {
            if (!(v.key = hardicon_key_code(arg, arg_len)))
                return false;
        } else if (arg_len > 0) {
            return false;
        }
        *a = v;
        return true;
    }
    return false;
}

void hardicon_parse(struct hardicon_config *c, const char *text, int len)
{
    int line = 0;
    hardicon_defaults(c);
    for (int i = 0; i < len && line <= HARDICON_COUNT;) {
        int end = i;
        while (end < len && text[end] != '\n')
            end++;
        int n = end - i;
        if (n && text[i + n - 1] == '\r')
            n--;
        if (line < HARDICON_COUNT)
            parse_line(&c->icon[line], text + i, n);
        else if (n == 6 && !strncmp(text + i, "apps ", 5) && (text[i + 5] == '0' || text[i + 5] == '1'))
            c->apps = text[i + 5] == '1';
        line++;
        i = end + 1;
    }
}

void hardicon_load(struct hardicon_config *c)
{
    char buf[512];
    int n = pref_get(HARDICON_PREF, buf, sizeof(buf));
    if (n > 0)
        hardicon_parse(c, buf, n);
    else
        hardicon_defaults(c);
}

int hardicon_save(const struct hardicon_config *c)
{
    char buf[512];
    int n = hardicon_format(c, buf, sizeof(buf));
    if (n >= (int)sizeof(buf))
        return GZ_EINVAL;
    return pref_put(HARDICON_PREF, buf, (uint32_t)n);
}
