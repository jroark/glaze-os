/* Offline, integer-only recognition of one printed ASCII character. */
#ifndef GLAZE_HANDWRITING_H
#define GLAZE_HANDWRITING_H
#include <glaze/types.h>
#define UI_INK_POINTS 256u
#define UI_INK_STROKES 8u
#define UI_INK_CHOICES 3u
struct ui_ink_point { int16_t x,y; uint16_t stroke; };
struct ui_ink {
    struct ui_ink_point points[UI_INK_POINTS];
    uint16_t count,strokes,start;
    bool drawing,overflow;
};
struct ui_ink_choice { char ch; uint32_t distance; };
void ui_ink_clear(struct ui_ink *ink);
/* Coordinates must be in 0..1023. No allocation; excess ink fails closed. */
bool ui_ink_begin(struct ui_ink *ink,int x,int y);
bool ui_ink_move(struct ui_ink *ink,int x,int y);
void ui_ink_end(struct ui_ink *ink,int x,int y);
void ui_ink_cancel(struct ui_ink *ink); /* discard only the current stroke */
/* Returns up to three distinct candidates, best first; 0 for blank,
 * incomplete, excessive or unrecognizable ink. numbers selects 0..9,
 * otherwise a..z. Case belongs to the input panel, not the recognizer. */
unsigned ui_ink_recognize(const struct ui_ink *,bool numbers,struct ui_ink_choice out[UI_INK_CHOICES]);
#endif
