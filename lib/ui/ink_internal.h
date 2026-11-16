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
/* Keyboard geometry is a 240 x 150 design, scaled to the keyboard's size:
 * ui_w() wide by UI_KB_H, or UI_KB_LAND_H in landscape (portrait is 1:1).
 * Drawing scales design rects out; touches come back in design coordinates,
 * so key hits and handwriting ink keep one layout. */
static inline int uikb_h(void){return ui_landscape()?UI_KB_LAND_H:UI_KB_H;}
static inline gfx_rect uikb_rect(gfx_rect d,int y)
{
    int w=ui_w(),h=uikb_h(),x0=d.x*w/UI_W,x1=(d.x+d.w)*w/UI_W,y0=d.y*h/UI_KB_H,y1=(d.y+d.h)*h/UI_KB_H;
    return gfx_r(x0,y+y0,x1-x0,y1-y0);
}
static inline int uikb_x(int x){return x*ui_w()/UI_W;}
static inline int uikb_y(int y){return y*uikb_h()/UI_KB_H;}
static inline int uikb_dx(int x){return x*UI_W/ui_w();}
static inline int uikb_dy(int y){return y*UI_KB_H/uikb_h();}
bool uikb_ink_down(void);
#endif
