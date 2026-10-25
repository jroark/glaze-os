#ifndef SB_DISPLAY_H
#define SB_DISPLAY_H
#include <glaze/display.h>

struct sb_display {
    struct display_settings saved, current;
    int owner;              /* transient slider preview; zero once saved */
};
void sb_display_load(struct sb_display *s);
void sb_display_cancel(struct sb_display *s);
int sb_display_request(struct sb_display *s, uint32_t op, int pid, int brightness, int contrast);
#endif
