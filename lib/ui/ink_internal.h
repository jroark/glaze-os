#ifndef GLAZE_UI_INK_INTERNAL_H
#define GLAZE_UI_INK_INTERNAL_H
#include <glaze/ui.h>
#include <glaze/handwriting.h>
/* All these symbols remain private to libui. */
unsigned uihw_examples(void);
char uihw_example(unsigned index,struct ui_ink *out);
void uihw_reset(const struct ui_keyboard *owner);
bool uihw_down(void);
void uihw_draw(gfx_surface *,const struct ui_keyboard *,int y);
int uihw_event(struct ui_keyboard *,const struct gz_input_event *,int y,bool *changed);
void uikb_release(void);
bool uikb_ink_down(void);
#endif
