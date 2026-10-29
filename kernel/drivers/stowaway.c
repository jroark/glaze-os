/* Stowaway.PPC300_4000.cab Stowaway.dll: 0x103dd4 checks FA FD;
 * 0x101c1c decodes bit 7 as release. US map follows be300/src/ui_stowaway.c
 * and web/app.js. This map is inferred, not validated on a physical dock.
 */
#include "stowaway.h"

static const char plain[96] = {
    [0]='1', [1]='2', [2]='3', [3]='z', [4]='4', [5]='5', [6]='6', [7]='7',
    [9]='q', [10]='w', [11]='e', [12]='r', [13]='t', [14]='y', [15]='`',
    [16]='x', [17]='a', [18]='s', [19]='d', [20]='f', [21]='g', [22]='h', [23]=' ',
    [44]='c', [45]='v', [46]='b', [47]='n', [48]='-', [49]='=',
    [52]='8', [53]='9', [54]='0', [56]='[', [57]=']', [58]='\\',
    [60]='u', [61]='i', [62]='o', [63]='p', [64]='\'', [68]='j', [69]='k',
    [70]='l', [71]=';', [72]='/', [76]='m', [77]=',', [78]='.'
};
static const char shifted[96] = {
    [0]='!', [1]='@', [2]='#', [4]='$', [5]='%', [6]='^', [7]='&', [15]='~',
    [48]='_', [49]='+', [52]='*', [53]='(', [54]=')', [56]='{', [57]='}',
    [58]='|', [64]='"', [71]=':', [72]='?', [77]='<', [78]='>'
};
static const uint16_t special[96] = {
    [25]=GZ_KEY_TAB, [50]=GZ_KEY_BACKSPACE, [51]=GZ_KEY_KBD_HOME,
    [55]=GZ_KEY_ESC, [59]=GZ_KEY_END, [65]=GZ_KEY_ENTER, [66]=GZ_KEY_PAGEUP,
    [73]=GZ_KEY_UP, [74]=GZ_KEY_PAGEDOWN, [79]=0, [80]=GZ_KEY_DELETE,
    [81]=GZ_KEY_LEFT, [82]=GZ_KEY_DOWN, [83]=GZ_KEY_RIGHT
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

static void emit_press(struct stowaway_decoder *s, unsigned key, stowaway_emit emit)
{
    bool shift = s->down[87] || s->down[88];
    if (special[key]) {
        unsigned code = special[key];
        if (shift) {
            if (code == GZ_KEY_LEFT) code = GZ_KEY_SELECT_LEFT;
            if (code == GZ_KEY_RIGHT) code = GZ_KEY_SELECT_RIGHT;
            if (code == GZ_KEY_KBD_HOME) code = GZ_KEY_SELECT_HOME;
            if (code == GZ_KEY_END) code = GZ_KEY_SELECT_END;
        }
        s->held[key] = code;
        emit(GZ_EV_KEY_DOWN, code);
        return;
    }
    unsigned ch = (unsigned char)plain[key];
    if (!ch || s->down[35]) return;
    if (s->down[26]) {
        unsigned code = ch == 'a' ? GZ_KEY_SELECT_ALL : ch == 'c' ? GZ_KEY_COPY :
                        ch == 'x' ? GZ_KEY_CUT : ch == 'v' ? GZ_KEY_PASTE : 0;
        if (code) { s->held[key] = code; emit(GZ_EV_KEY_DOWN, code); }
        return; /* Never insert Ctrl letters into the document. */
    }
    if (ch >= 'a' && ch <= 'z') {
        if (shift != s->caps)
            ch -= 'a' - 'A';
    } else if (shift && shifted[key]) {
        ch = (unsigned char)shifted[key];
    }
    emit(GZ_EV_TEXT, (uint16_t)ch);
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
    if (key == 24)
        s->caps = !s->caps;
    emit_press(s, key, emit);
    if (plain[key] || (special[key] && special[key] != GZ_KEY_ESC &&
                      special[key] != GZ_KEY_ENTER && special[key] != GZ_KEY_TAB)) {
        s->repeat = (int)key + 1;     /* zero means none; scancode 0 is valid */
        s->deadline = now + 450;
    }
}

void stowaway_repeat(struct stowaway_decoder *s, uint32_t now, stowaway_emit emit)
{
    if (s->repeat && (int32_t)(now - s->deadline) >= 0) {
        emit_press(s, (unsigned)(s->repeat - 1), emit);
        s->deadline = now + 50;      /* don't flood after a stalled consumer */
    }
}
