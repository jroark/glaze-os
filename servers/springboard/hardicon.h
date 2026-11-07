#ifndef SB_HARDICON_H
#define SB_HARDICON_H
#include <glaze/hardicon.h>

/* A stroke that starts on the icon strip belongs to SpringBoard; one that
 * starts on the LCD stays with its owner even when it slides down there.
 * A strip stroke fires only if it was pressed clear of the LCD, lasted at
 * least HARDICON_MIN_MS and never left its icon (docs/os/HARD_ICONS.md). */
struct sb_hardicon {
    bool active;
    int zone;
    uint32_t down_ms;
    int down_x, down_y;
    const char *reject;     /* why the stroke can't fire, or NULL */
};
/* x, y: the touch's calibrated point, before clamping to the LCD. Returns
 * true when SpringBoard keeps the event; *fire is then the icon released
 * on, or -1. After a kept UP, s->reject says why it didn't fire. */
bool sb_hardicon_input(struct sb_hardicon *s, const struct gz_input_event *e, int x, int y, int *fire);
#endif
