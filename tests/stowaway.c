/* Native tests for real byte decoding and cursor editing, no MIPS/MMIO. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <glaze/ui.h>
#include "../kernel/drivers/stowaway.h"
struct ui_app ui;
static struct gz_input_event events[128];
static int count;
static void emit(uint16_t type, uint16_t code) {
    assert(count < 128);
    events[count++] = (struct gz_input_event){ .type=type, .code=code };
}
static void tap(struct stowaway_decoder *s, int key, uint32_t now) {
    stowaway_decode(s, (uint8_t)key, now, emit);
    stowaway_decode(s, (uint8_t)(key | 128), now + 1, emit);
}
static void expect(int i, int type, int code) {
    assert(i < count && events[i].type == type && events[i].code == code);
}
static void decoder_tests(void) {
    unsigned ack = 0;
    assert(!stowaway_ack(&ack, 0xfd));
    assert(!stowaway_ack(&ack, 0xfa));
    assert(!stowaway_ack(&ack, 0));
    assert(!stowaway_ack(&ack, 0xfd));
    assert(!stowaway_ack(&ack, 0xfa));
    assert(!stowaway_ack(&ack, 0xfa));
    assert(stowaway_ack(&ack, 0xfd));
    struct stowaway_decoder s = {0};
    tap(&s, 0, 0); expect(0, GZ_EV_TEXT, '1'); /* zero isn't a sentinel */
    tap(&s, 17, 2); expect(1, GZ_EV_TEXT, 'a');
    stowaway_decode(&s, 87, 3, emit);
    stowaway_decode(&s, 88, 3, emit);
    stowaway_decode(&s, 87 | 128, 4, emit);
    tap(&s, 17, 5); expect(2, GZ_EV_TEXT, 'A');
    tap(&s, 0, 6); expect(3, GZ_EV_TEXT, '!');
    stowaway_decode(&s, 88 | 128, 7, emit);
    stowaway_decode(&s, 24, 8, emit);
    stowaway_decode(&s, 24, 8, emit); /* duplicate caps must not toggle */
    stowaway_decode(&s, 24 | 128, 9, emit);
    tap(&s, 17, 10); expect(4, GZ_EV_TEXT, 'A');
    stowaway_decode(&s, 87, 11, emit);
    tap(&s, 17, 12); expect(5, GZ_EV_TEXT, 'a');
    stowaway_decode(&s, 87 | 128, 13, emit);
    int n = count;
    stowaway_decode(&s, 26, 14, emit); tap(&s, 17, 15);
    stowaway_decode(&s, 26 | 128, 16, emit);
    stowaway_decode(&s, 35, 17, emit); tap(&s, 17, 18);
    stowaway_decode(&s, 35 | 128, 19, emit);
    tap(&s, 127, 20); tap(&s, 105, 20); /* bad/unmapped bytes */
    assert(count == n);
    tap(&s, 51, 21);
    expect(n, GZ_EV_KEY_DOWN, GZ_KEY_KBD_HOME);
    expect(n + 1, GZ_EV_KEY_UP, GZ_KEY_KBD_HOME);
    assert(GZ_KEY_KBD_HOME != GZ_KEY_HOME);
    s = (struct stowaway_decoder){0}; count = 0;
    stowaway_decode(&s, 0, 100, emit);
    stowaway_decode(&s, 0, 101, emit);
    stowaway_repeat(&s, 549, emit); assert(count == 1);
    stowaway_repeat(&s, 550, emit); expect(1, GZ_EV_TEXT, '1');
    stowaway_repeat(&s, 599, emit); assert(count == 2);
    stowaway_repeat(&s, 600, emit); assert(count == 3);
    stowaway_decode(&s, 128, 601, emit);
    stowaway_repeat(&s, 2000, emit); assert(count == 3);
    count = 0;
    tap(&s, 65, 2001); stowaway_repeat(&s, 3000, emit);
    assert(count == 2); /* Enter never repeats */
    s = (struct stowaway_decoder){0}; count = 0;
    stowaway_decode(&s, 50, UINT32_MAX - 100, emit);
    stowaway_repeat(&s, 348, emit); assert(count == 1);
    stowaway_repeat(&s, 349, emit); assert(count == 2);
    stowaway_decode(&s, 50 | 128, 350, emit);
    stowaway_repeat(&s, 1000, emit); assert(count == 3);
}
static bool key(char *buf, int cap, int *pos, int code, bool multi) {
    struct gz_input_event e = {.type=GZ_EV_KEY_DOWN, .code=(uint16_t)code};
    return ui_edit_input(buf, cap, pos, &e, multi);
}
static void edit_tests(void) {
    char buf[16] = "ac"; int p = 1;
    struct gz_input_event e = {.type=GZ_EV_TEXT, .code='b'};
    assert(ui_edit_input(buf, sizeof(buf), &p, &e, false));
    assert(!strcmp(buf, "abc") && p == 2);
    key(buf, sizeof(buf), &p, GZ_KEY_BACKSPACE, false);
    assert(!strcmp(buf, "ac") && p == 1);
    key(buf, sizeof(buf), &p, GZ_KEY_DELETE, false);
    assert(!strcmp(buf, "a") && p == 1);
    key(buf, sizeof(buf), &p, GZ_KEY_KBD_HOME, false); assert(p == 0);
    key(buf, sizeof(buf), &p, GZ_KEY_END, false); assert(p == 1);
    assert(!key(buf, sizeof(buf), &p, GZ_KEY_ENTER, false));
    key(buf, sizeof(buf), &p, GZ_KEY_ENTER, true); assert(!strcmp(buf, "a\n"));
    strcpy(buf, "ab\ncde\nf"); p = 5;
    key(buf, sizeof(buf), &p, GZ_KEY_UP, true); assert(p == 2);
    key(buf, sizeof(buf), &p, GZ_KEY_DOWN, true); assert(p == 5);
    key(buf, sizeof(buf), &p, GZ_KEY_KBD_HOME, true); assert(p == 3);
    key(buf, sizeof(buf), &p, GZ_KEY_END, true); assert(p == 6);
    strcpy(buf, "abc"); p = 3;
    ui_edit_input(buf, 4, &p, &e, false); assert(!strcmp(buf, "abc"));
    e.type = GZ_EV_KEY_UP; assert(!ui_edit_input(buf, sizeof(buf), &p, &e, false));
    e.type = GZ_EV_TOUCH_UP; assert(!ui_edit_input(buf, sizeof(buf), &p, &e, false));
    ui.hardware_keyboard = false; assert(ui_keyboard_height() == UI_KB_H);
    ui.hardware_keyboard = true; assert(ui_keyboard_height() == 0);
}
int main(void) {
    decoder_tests(); edit_tests();
    puts("stowaway: ALL PASS");
    return 0;
}
