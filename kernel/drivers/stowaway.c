/* Stowaway.PPC300_4000.cab Stowaway.dll: 0x103dd4 checks FA FD;
 * 0x101c1c decodes bit 7 as release. Scancodes follow the DLL's US tables
 * (be300/src/stowaway_keys.def) and were checked on a physical Targus
 * Stowaway (tests/STOWAWAY.md): modifiers at 0x10caa0 are 8 Fn, 24 Caps
 * Lock, 26 Ctrl, 34 Windows, 35 Alt, 88/89 Shift. The keyboard's legends
 * give the rest: 55 is a second Space key, 79 Today, and 51/59/66/74 the
 * Inbox, Contacts, Calendar and Tasks app keys (unmapped). Fn turns the
 * arrows into Home/End/PgUp/PgDn (the DLL's Fn layer), Enter into OK and
 * Backspace into Off, and Caps Lock into Numlock, the printed keypad.
 */
#include "stowaway.h"

#define K_FN        8
#define K_CAPS      24
#define K_CTRL      26
#define K_ALT       35
#define K_ENTER     65
#define K_BACKSPACE 50

static const char plain[96] = {
    [0]='1', [1]='2', [2]='3', [3]='z', [4]='4', [5]='5', [6]='6', [7]='7',
    [9]='q', [10]='w', [11]='e', [12]='r', [13]='t', [14]='y', [15]='`',
    [16]='x', [17]='a', [18]='s', [19]='d', [20]='f', [21]='g', [22]='h', [23]=' ',
    [44]='c', [45]='v', [46]='b', [47]='n', [48]='-', [49]='=',
    [52]='8', [53]='9', [54]='0', [55]=' ', [56]='[', [57]=']', [58]='\\',
    [60]='u', [61]='i', [62]='o', [63]='p', [64]='\'', [68]='j', [69]='k',
    [70]='l', [71]=';', [72]='/', [76]='m', [77]=',', [78]='.'
};
static const char shifted[96] = {
    [0]='!', [1]='@', [2]='#', [4]='$', [5]='%', [6]='^', [7]='&', [15]='~',
    [48]='_', [49]='+', [52]='*', [53]='(', [54]=')', [56]='{', [57]='}',
    [58]='|', [64]='"', [71]=':', [72]='?', [77]='<', [78]='>'
};
/* Numlock: the keypad printed in yellow on the keys. */
static const char keypad[96] = {
    [7]='7', [52]='8', [53]='9', [54]='/', [60]='4', [61]='5', [62]='6',
    [63]='*', [68]='1', [69]='2', [70]='3', [71]='-', [76]='0', [78]='.',
    [72]='+'
};
static const uint16_t special[96] = {
    [25]=GZ_KEY_TAB, [50]=GZ_KEY_BACKSPACE, [65]=GZ_KEY_ENTER,
    [73]=GZ_KEY_UP, [79]=GZ_KEY_HOME, [80]=GZ_KEY_DELETE,
    [81]=GZ_KEY_LEFT, [82]=GZ_KEY_DOWN, [83]=GZ_KEY_RIGHT
};
static const uint16_t fn_special[96] = {
    [50]=GZ_KEY_POWER, [65]=GZ_KEY_OK,
    [73]=GZ_KEY_PAGEUP, [81]=GZ_KEY_KBD_HOME, [82]=GZ_KEY_PAGEDOWN, [83]=GZ_KEY_END
};

bool stowaway_ack(unsigned *state, uint8_t byte)
{
    if (*state == 1 && byte == 0xfd) {
        *state = 2;
        return true;
    }
    *state = byte == 0xfa ? 1 : 0;
    return false;
}

/* Emits a key's press; returns whether holding it repeats. */
static bool emit_press(struct stowaway_decoder *s, unsigned key, stowaway_emit emit)
{
    bool shift = s->down[88] || s->down[89];
    unsigned code = s->down[K_FN] && fn_special[key] ? fn_special[key] : special[key];
    if (code) {
        if (shift) {
            if (code == GZ_KEY_LEFT) code = GZ_KEY_SELECT_LEFT;
            if (code == GZ_KEY_RIGHT) code = GZ_KEY_SELECT_RIGHT;
            if (code == GZ_KEY_KBD_HOME) code = GZ_KEY_SELECT_HOME;
            if (code == GZ_KEY_END) code = GZ_KEY_SELECT_END;
        }
        s->held[key] = code;
        emit(GZ_EV_KEY_DOWN, code);
        return code != GZ_KEY_ENTER && code != GZ_KEY_TAB && code != GZ_KEY_OK &&
               code != GZ_KEY_POWER && code != GZ_KEY_HOME;
    }
    unsigned ch = (unsigned char)plain[key];
    if (!ch || s->down[K_ALT]) return false;
    if (s->down[K_CTRL]) {
        code = ch == 'a' ? GZ_KEY_SELECT_ALL : ch == 'c' ? GZ_KEY_COPY :
               ch == 'x' ? GZ_KEY_CUT : ch == 'v' ? GZ_KEY_PASTE : 0;
        if (code) { s->held[key] = code; emit(GZ_EV_KEY_DOWN, code); }
        return false; /* Never insert Ctrl letters into the document. */
    }
    if (s->numlock && keypad[key]) {
        ch = (unsigned char)keypad[key];
    } else if (ch >= 'a' && ch <= 'z') {
        if (shift != s->caps)
            ch -= 'a' - 'A';
    } else if (shift && shifted[key]) {
        ch = (unsigned char)shifted[key];
    }
    emit(GZ_EV_TEXT, (uint16_t)ch);
    return true;
}

void stowaway_decode(struct stowaway_decoder *s, uint8_t byte, uint32_t now, stowaway_emit emit)
{
    unsigned key = byte & 0x7f;
    bool release = (byte & 0x80) != 0;
    if (key >= 96 || s->down[key] == !release)
        return;
    s->down[key] = !release;
    if (release) {
        if (s->repeat == (int)key + 1)
            s->repeat = 0;
        if (s->held[key]) emit(GZ_EV_KEY_UP, s->held[key]);
        s->held[key] = 0;
        return;
    }
    if (key == K_CAPS) {
        if (s->down[K_FN])
            s->numlock = !s->numlock;
        else
            s->caps = !s->caps;
    }
    if (emit_press(s, key, emit)) {
        s->repeat = (int)key + 1;     /* zero means none; scancode 0 is valid */
        s->deadline = now + 450;
    }
}

void stowaway_repeat(struct stowaway_decoder *s, uint32_t now, stowaway_emit emit)
{
    if (s->repeat && (int32_t)(now - s->deadline) >= 0) {
        (void)emit_press(s, (unsigned)(s->repeat - 1), emit);
        s->deadline = now + 50;      /* don't flood after a stalled consumer */
    }
}
