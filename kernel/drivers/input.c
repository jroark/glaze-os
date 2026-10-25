/*
 * Touch panel (VRC4173 PIU) and button input.
 *
 * Touch is interrupt driven: each converted coordinate page becomes a
 * down/move event, and a PENCHG with the pen up becomes an up event.
 * Buttons are sampled from every timer tick and on their GIRQ0 source.
 * Events go into a ring that the bound (compositor) port drains.
 */

#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#include "../proc.h"

/* Raw ADC endpoints (doc: Touch Panel, "Linux input driver guidance"). */
#define ADC_Y_MIN   0x81E1u
#define ADC_Y_MAX   0x8E30u
#define ADC_X_MIN   0x8300u
#define ADC_X_MAX   0x8D1Bu
#define PANEL_H     360         /* rows 320..359 are the printed icon strip */

#define RING_SIZE   64

/* Power on and scan while the pen is down (WaitPenTouch mode): start at
 * pen-down and stop at pen-up, as Windows CE runs it (CNT 0x1304 in the
 * idle dump) and NEC's driver (0x0326). Without PADATSTOP a real BE-300's
 * sequencer stopped in state 6 after the first touch. */
#define PIU_SCAN    (PIUCNT_PIUPWR | PIUCNT_PIUSEQEN | PIUCNT_PADATSTART | PIUCNT_PADATSTOP)

static struct gz_input_event ring[RING_SIZE];
static uint32_t ring_head, ring_tail;
static int bound_port;
static bool pen_down;
static int pen_x, pen_y;

/*
 * Touch filtering for the resistive panel. A real BE-300's samples jump by
 * more than a tap's slop as the contact settles and again at lift-off, so
 * a still tap read as the start of a swipe (the emulator's are exact). As
 * in tslib: drop samples without the valid flag (bit 15 of each ADC word,
 * as NEC's vrc4173-piu.c checks), drop the first after pen-down, hold the
 * latest back so the lift-off one is never reported, average the last
 * FILTER_N, and only report a move of at least MOVE_MIN pixels.
 */
#define ADC_VALID       0x8000u
#define FILTER_N        3
#define MOVE_MIN        2

static int stroke_samples;              /* valid samples since pen-down */
static int first_x, first_y;            /* the dropped settling sample */
static int win_x[FILTER_N], win_y[FILTER_N], win_n;
static bool held;                       /* a filtered sample not yet reported */
static int held_x, held_y;
static uint8_t keys1, keys2;

void input_push(uint16_t type, uint16_t code, int x, int y)
{
    uint32_t now = (uint32_t)(time_us() / 1000u);

    /* Coalesce consecutive moves so a busy compositor sees the latest. */
    if (type == GZ_EV_TOUCH_MOVE && ring_head != ring_tail) {
        struct gz_input_event *last = &ring[(ring_head - 1) % RING_SIZE];
        if (last->type == GZ_EV_TOUCH_MOVE) {
            last->x = (int16_t)x;
            last->y = (int16_t)y;
            last->time_ms = now;
            return;
        }
    }
    if (ring_head - ring_tail >= RING_SIZE)
        ring_tail++;            /* drop the oldest */
    struct gz_input_event *e = &ring[ring_head % RING_SIZE];
    e->type = type;
    e->code = code;
    e->x = (int16_t)x;
    e->y = (int16_t)y;
    e->time_ms = now;
    ring_head++;
    if (bound_port > 0)
        ipc_notify(bound_port, GZ_NOTIFY_INPUT);
}

static int adc_pos(uint16_t v, uint16_t lo, uint16_t hi, int span, bool invert)
{
    /* Keep out-of-factory-range positions for calibration. SpringBoard
     * applies the saved correction before clamping to the LCD. */
    int d = invert ? (int)hi - v : (int)v - lo;
    return d * (span - 1) / (hi - lo);
}

static bool adc_ok(uint16_t v, uint16_t lo, uint16_t hi)
{
    return v >= lo && v <= hi;
}

static void stroke_reset(void)
{
    stroke_samples = 0;
    win_n = 0;
    held = false;
}

static void report(int x, int y)
{
    if (!pen_down) {
        input_push(GZ_EV_TOUCH_DOWN, 0, x, y);
    } else {
        int dx = x - pen_x, dy = y - pen_y;
        if (dx < MOVE_MIN && dx > -MOVE_MIN && dy < MOVE_MIN && dy > -MOVE_MIN)
            return;
        input_push(GZ_EV_TOUCH_MOVE, 0, x, y);
    }
    pen_down = true;
    pen_x = x;
    pen_y = y;
}

static void touch_sample(int x, int y)
{
    if (stroke_samples++ == 0) {
        first_x = x;
        first_y = y;
        return;
    }
    int i = win_n++ % FILTER_N, n = win_n < FILTER_N ? win_n : FILTER_N, sx = 0, sy = 0;
    win_x[i] = x;
    win_y[i] = y;
    for (int k = 0; k < n; k++) {
        sx += win_x[k];
        sy += win_y[k];
    }
    if (held)
        report(held_x, held_y);
    held = true;
    held_x = sx / n;
    held_y = sy / n;
}

/* Pen-up: the held sample is the lift-off one and is dropped, unless the
 * tap was too short to report anything, which then lands where it began. */
static void touch_release(void)
{
    if (!pen_down && stroke_samples) {
        if (held)
            report(held_x, held_y);
        else
            report(first_x, first_y);
    }
    if (pen_down) {
        pen_down = false;
        input_push(GZ_EV_TOUCH_UP, 0, pen_x, pen_y);
    }
    stroke_reset();
}

static void read_page(uint32_t base)
{
    uint16_t yp = REG16(base + 0x0), ym = REG16(base + 0x4);
    uint16_t xm = REG16(base + 0x8), xp = REG16(base + 0xC);
    int x = 0, y = 0, n = 0;

    if (!(yp & ym & xm & xp & ADC_VALID))
        return;

    /* Average the +/- plates when both are in range. */
    if (adc_ok(xp, ADC_X_MIN, ADC_X_MAX)) {
        x += adc_pos(xp, ADC_X_MIN, ADC_X_MAX, FB_WIDTH, false);
        n++;
    }
    if (adc_ok(xm, ADC_X_MIN, ADC_X_MAX)) {
        x += adc_pos(xm, ADC_X_MIN, ADC_X_MAX, FB_WIDTH, true);
        n++;
    }
    x = n ? x / n : adc_pos(xp, ADC_X_MIN, ADC_X_MAX, FB_WIDTH, false);
    n = 0;
    if (adc_ok(yp, ADC_Y_MIN, ADC_Y_MAX)) {
        y += adc_pos(yp, ADC_Y_MIN, ADC_Y_MAX, PANEL_H, false);
        n++;
    }
    if (adc_ok(ym, ADC_Y_MIN, ADC_Y_MAX)) {
        y += adc_pos(ym, ADC_Y_MIN, ADC_Y_MAX, PANEL_H, true);
        n++;
    }
    y = n ? y / n : adc_pos(yp, ADC_Y_MIN, ADC_Y_MAX, PANEL_H, false);

    touch_sample(x, y);
}

static void touch_irq(void)
{
    uint16_t status = PIU_INT;
    uint16_t mask = status >> 8;
    uint16_t active = status & mask & 0xFF;
    uint16_t ack = active & PIUINT_OTHER;

    /* A new stroke starts clean; a short tap can deliver page data and
     * pen-up together, so the coordinates come before the release. */
    if ((active & PIUINT_PENCHG) && (PIU_CNT & PIUCNT_PENDOWN) && !pen_down)
        stroke_reset();
    if (active & PIUINT_PAGE0) {
        read_page(PIU_PAGE0);
        ack |= PIUINT_PAGE0;
    }
    if (active & PIUINT_PAGE1) {
        read_page(PIU_PAGE1);
        ack |= PIUINT_PAGE1;
    }
    if (active & PIUINT_PENCHG) {
        if (!(PIU_CNT & PIUCNT_PENDOWN))
            touch_release();
        ack |= PIUINT_PENCHG;
    }
    if (active & PIUINT_DATALOST) {
        /* Both pages filled before we drained them, and the sequencer
         * stopped. It restarts only once both pages and DATALOST are clear
         * and PIUCNT is rewritten (VRC4173 UM 9.7). */
        PIU_INT = (uint16_t)((mask << 8) | ack | PIUINT_DATALOST |
                             PIUINT_PAGE0 | PIUINT_PAGE1);
        PIU_CNT = PIU_SCAN;
        return;
    }
    if (ack)
        PIU_INT = (uint16_t)((mask << 8) | ack);
    /* Re-arm the scan after every interrupt, as NEC's VRC4173 PIU driver
     * does (linux4be 2.4 drivers/char/vrc4173-piu.c rewrites PIUCNTREG at
     * the end of its handler); a no-op while the sequencer is scanning. */
    PIU_CNT = PIU_SCAN;
}

static void key_edges(uint8_t now, uint8_t prev, uint8_t bit, uint16_t code)
{
    if ((now ^ prev) & bit)
        input_push((now & bit) ? GZ_EV_KEY_DOWN : GZ_EV_KEY_UP, code, 0, 0);
}

#ifdef GLAZE_INPUT_SCRIPT
void input_script_load(void);
void input_script_poll(void);
#endif

void input_poll(void)
{
    stowaway_poll();
#ifdef GLAZE_INPUT_SCRIPT
    input_script_poll();
#endif
    (void)KEYS_LATCH;
    uint8_t s1 = KEYS_SET1, s2 = KEYS_SET2;

    if (s1 == keys1 && s2 == keys2)
        return;
    key_edges(s1, keys1, KEY1_OK, GZ_KEY_OK);
    key_edges(s1, keys1, KEY1_ESC, GZ_KEY_ESC);
    key_edges(s1, keys1, KEY1_UP, GZ_KEY_UP);
    key_edges(s1, keys1, KEY1_DOWN, GZ_KEY_DOWN);
    key_edges(s1, keys1, KEY1_LEFT, GZ_KEY_LEFT);
    key_edges(s1, keys1, KEY1_RIGHT, GZ_KEY_RIGHT);
    key_edges(s2, keys2, KEY2_ROCKET, GZ_KEY_HOME);
    key_edges(s2, keys2, KEY2_POWER, GZ_KEY_POWER);
    keys1 = s1;
    keys2 = s2;
}

void input_irq(uint16_t kj_status)
{
    if (kj_status & KJ_INT_TOUCH)
        touch_irq();
    if (kj_status & KJ_INT_KEYS)
        input_poll();
}

void input_init(void)
{
    (void)KEYS_LATCH;
    keys1 = KEYS_SET1;
    keys2 = KEYS_SET2;

    /* Windows CE's settings from the idle dump (hw_dump_vrc4173.txt
     * 0x0A000300): a 0x5DC interval (30 us units, ~22 Hz) and stable count
     * 0xC8, and interrupt mask 0xDD. A ~10 ms interval lost data on the
     * first touch on a real BE-300. */
    PIU_SIVL = 0x05DC;
    PIU_STBL = 0x00C8;
    PIU_INT = (PIUINT_PENCHG | PIUINT_DATALOST | PIUINT_PAGE0 | PIUINT_PAGE1 | PIUINT_OTHER) << 8;
    PIU_CNT = PIU_SCAN;
    stowaway_init();
#ifdef GLAZE_INPUT_SCRIPT
    input_script_load();
#endif
}

int input_bind(int port)
{
    bound_port = port;
    if (ring_head != ring_tail)
        ipc_notify(port, GZ_NOTIFY_INPUT);
    return 0;
}

int input_read(struct addrspace *as, uint32_t uevents, uint32_t max)
{
    uint32_t n = 0;

    while (n < max && ring_tail != ring_head) {
        if (copy_to_as(as, uevents + n * sizeof(struct gz_input_event),
                       &ring[ring_tail % RING_SIZE], sizeof(struct gz_input_event)))
            return GZ_EINVAL;
        ring_tail++;
        n++;
    }
    return (int)n;
}

#ifdef GLAZE_INPUT_SCRIPT
/*
 * Test builds only: replay a timed input script from romfs "test/input.txt"
 * so UI flows can be exercised without a person at the emulator window.
 *
 *   <ms> down|move|up <x> <y>
 *   <ms> tap <x> <y>
 *   <ms> drag <x0> <y0> <x1> <y1> <duration_ms>
 *   <ms> key ok|esc|up|down|left|right|home|power
 */
struct script_ev {
    uint32_t t;
    uint16_t type, code;
    int16_t x, y;
};

#define SCRIPT_MAX 512
static struct script_ev script[SCRIPT_MAX];
static int script_n, script_pos;

static const char *parse_int(const char *p, int *out)
{
    int v = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    *out = v;
    return p;
}

static void script_add(uint32_t t, uint16_t type, uint16_t code, int x, int y)
{
    if (script_n < SCRIPT_MAX)
        script[script_n++] = (struct script_ev){ t, type, code, (int16_t)x, (int16_t)y };
}

static bool word_is(const char *p, const char *w)
{
    size_t n = strlen(w);
    return !strncmp(p, w, n) && (p[n] == ' ' || p[n] == '\n' || p[n] == '\r' || !p[n]);
}

void input_script_load(void)
{
    static const struct { const char *name; uint16_t code; } keys[] = {
        { "ok", GZ_KEY_OK }, { "esc", GZ_KEY_ESC }, { "up", GZ_KEY_UP }, { "down", GZ_KEY_DOWN },
        { "left", GZ_KEY_LEFT }, { "right", GZ_KEY_RIGHT }, { "home", GZ_KEY_HOME }, { "power", GZ_KEY_POWER },
        { "enter", GZ_KEY_ENTER }, { "backspace", GZ_KEY_BACKSPACE },
    };
    const struct romfs_entry *e = romfs_lookup("test/input.txt");
    char *text = e ? page_alloc_n(ALIGN_UP(e->size + 1, PAGE_SIZE) / PAGE_SIZE) : NULL;

    /* Parsed from a NUL-terminated copy; the events keep no pointers into it. */
    if (!text || romfs_read(e, 0, text, e->size))
        return;
    const char *p = text, *end = text + e->size;
    while (p < end) {
        int t, x, y, x1, y1, dur;
        if (*p == '#' || *p == '\n' || *p == '\r') {
            while (p < end && *p++ != '\n')
                ;
            continue;
        }
        p = parse_int(p, &t);
        while (*p == ' ')
            p++;
        if (word_is(p, "down") || word_is(p, "move") || word_is(p, "up")) {
            uint16_t type = p[0] == 'd' ? GZ_EV_TOUCH_DOWN : p[0] == 'm' ? GZ_EV_TOUCH_MOVE : GZ_EV_TOUCH_UP;
            p = parse_int(parse_int(p + (type == GZ_EV_TOUCH_UP ? 2 : 4), &x), &y);
            script_add((uint32_t)t, type, 0, x, y);
        } else if (word_is(p, "tap")) {
            p = parse_int(parse_int(p + 3, &x), &y);
            script_add((uint32_t)t, GZ_EV_TOUCH_DOWN, 0, x, y);
            script_add((uint32_t)t + 80, GZ_EV_TOUCH_UP, 0, x, y);
        } else if (word_is(p, "drag")) {
            p = parse_int(parse_int(parse_int(parse_int(parse_int(p + 4, &x), &y), &x1), &y1), &dur);
            int steps = dur / 30 + 1;
            script_add((uint32_t)t, GZ_EV_TOUCH_DOWN, 0, x, y);
            for (int i = 1; i <= steps; i++)
                script_add((uint32_t)(t + dur * i / steps), GZ_EV_TOUCH_MOVE, 0,
                           x + (x1 - x) * i / steps, y + (y1 - y) * i / steps);
            script_add((uint32_t)(t + dur + 20), GZ_EV_TOUCH_UP, 0, x1, y1);
        } else if (word_is(p, "text")) {
            /* The rest of the line, typed as GZ_EV_TEXT 40 ms apart. */
            for (p += 5; p < end && *p != '\n' && *p != '\r'; p++, t += 40)
                script_add((uint32_t)t, GZ_EV_TEXT, (uint8_t)*p, 0, 0);
        } else if (word_is(p, "key")) {
            p += 4;
            for (unsigned k = 0; k < ARRAY_SIZE(keys); k++) {
                if (word_is(p, keys[k].name)) {
                    script_add((uint32_t)t, GZ_EV_KEY_DOWN, keys[k].code, 0, 0);
                    script_add((uint32_t)t + 60, GZ_EV_KEY_UP, keys[k].code, 0, 0);
                }
            }
        }
        while (p < end && *p++ != '\n')
            ;
    }
    kprintf("input: replaying %d scripted events\n", script_n);
}

void input_script_poll(void)
{
    uint32_t now = (uint32_t)(time_us() / 1000u);

    while (script_pos < script_n && script[script_pos].t <= now) {
        const struct script_ev *e = &script[script_pos++];
        input_push(e->type, e->code, e->x, e->y);
    }
}
#endif
