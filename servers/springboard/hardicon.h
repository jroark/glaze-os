#ifndef SB_HARDICON_H
#define SB_HARDICON_H
#include <glaze/hardicon.h>

/* A stroke that starts on the icon strip belongs to SpringBoard; one that
 * starts on the LCD stays with its owner even when it slides down there. */
struct sb_hardicon {
    bool active;
    int zone;
};
/* x, y: the touch's calibrated point, before clamping to the LCD. Returns
 * true when SpringBoard keeps the event; *fire is then the icon released
 * on, or -1. */
bool sb_hardicon_input(struct sb_hardicon *s, const struct gz_input_event *e, int x, int y, int *fire);
#endif
