#include "../apps/wince/number.h"
#include "../apps/wince/property_state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct mock {
    unsigned made, shown, applied, reset;
    int veto_leave, veto_enter, invalid_apply;
    bool veto_cancel;
};
static bool prepare(void *ctx, unsigned index)
{
    struct mock *m = ctx;
    m->made |= 1u << index;
    return true;
}
static int32_t notify(void *ctx, unsigned index, int code, bool closing)
{
    struct mock *m = ctx;
    assert(m->made & (1u << index));
    if (code == CE_PSN_LEAVE)
        return m->veto_leave;
    if (code == CE_PSN_QUERY_CANCEL)
        return m->veto_cancel;
    if (code == CE_PSN_ACTIVE)
        return m->veto_enter == (int)index;
    if (code == CE_PSN_APPLY) {
        assert(closing);
        m->applied |= 1u << index;
        return m->invalid_apply == (int)index;
    }
    if (code == CE_PSN_RESET)
        m->reset |= 1u << index;
    return 0;
}
static void show(void *ctx, unsigned index, bool visible)
{
    struct mock *m = ctx;
    if (visible)
        m->shown |= 1u << index;
    else
        m->shown &= ~(1u << index);
}
static void number(const char *s, bool sign, bool ok, uint32_t expected)
{
    uint16_t text[80];
    unsigned n;
    for (n = 0; s[n]; n++)
        text[n] = (unsigned char)s[n];
    text[n] = 0;
    uint32_t value = 123;
    assert(ce_number_parse(text, sign, &value) == ok);
    assert(value == (ok ? expected : 123));
}
int main(void)
{
    struct mock m = {.veto_enter = -1, .invalid_apply = -1};
    struct ce_property_state s = {3, 0, -1, prepare, notify, show, &m};
    assert(!ce_property_select(&s, 3));
    assert(ce_property_select(&s, 0));
    assert(s.active == 0 && m.shown == 1);
    assert(ce_property_select(&s, 1));
    assert(s.active == 1 && m.shown == 2 && m.made == 3);
    m.veto_leave = 1;
    assert(!ce_property_select(&s, 2));
    assert(m.made == 3 && s.active == 1);
    assert(!ce_property_apply(&s, true));
    assert(!m.applied);
    m.veto_leave = 0;
    m.veto_enter = 2;
    assert(!ce_property_select(&s, 2));
    assert(s.active == 1 && m.shown == 2 && m.made == 7);
    m.veto_enter = -1;
    m.invalid_apply = 0;
    assert(!ce_property_apply(&s, true));
    assert(s.active == 0 && m.shown == 1 && m.applied == 1);
    m.invalid_apply = -1;
    m.applied = 0;
    assert(ce_property_apply(&s, true));
    assert(m.applied == 7);
    m.veto_cancel = true;
    assert(!ce_property_cancel(&s));
    assert(!m.reset);
    m.veto_cancel = false;
    assert(ce_property_cancel(&s));
    assert(m.reset == 7);
    /* A page that was never instantiated must not write defaults on Apply. */
    memset(&m, 0, sizeof(m));
    m.veto_enter = m.invalid_apply = -1;
    s.created = 0;
    s.active = -1;
    assert(ce_property_select(&s, 0));
    assert(ce_property_apply(&s, true));
    assert(m.applied == 1);
    number("0", false, true, 0);
    number("  +42px", true, true, 42);
    number("-2147483648", true, true, 0x80000000u);
    number("2147483647", true, true, 0x7fffffffu);
    number("4294967295", false, true, 0xffffffffu);
    number("4294967296", false, false, 0);
    number("2147483648", true, false, 0);
    number("-2147483649", true, false, 0);
    number("-1", false, false, 0);
    number("+", true, false, 0);
    number("", false, false, 0);
    number("text", false, false, 0);
    uint16_t text[12];
    uint32_t value;
    for (unsigned i = 0; i < 10000; i++) {
        uint32_t n = i * 429497u;
        ce_number_format(text, n, false);
        assert(ce_number_parse(text, false, &value) && value == n);
        ce_number_format(text, n, true);
        assert(ce_number_parse(text, true, &value) && value == n);
    }
    puts("WinCE property-page validation/apply/cancel and numeric boundaries passed");
}
