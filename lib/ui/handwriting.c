/* 240x150 handwriting panel behind the existing ui_keyboard API. */
#include "ink_internal.h"
#include "sound.h"
#include <glaze/audio.h>
#include <string.h>
enum { C0,C1,C2,SHIFT,NUMBERS,HELP,KEYS,CLEAR,SPACE,BACK,ENTER,SLASH,DRAW=-2 };
static struct {
    const struct ui_keyboard *owner;
    struct ui_ink ink;
    struct ui_ink_choice choices[UI_INK_CHOICES];
    unsigned count;
    bool gesture,blocked,help;
} hw;
void uihw_reset(const struct ui_keyboard *owner)
{if(!owner && !hw.owner)return;memset(&hw,0,sizeof(hw));hw.owner=owner;}
bool uihw_down(void){return hw.ink.drawing;}
static void own(const struct ui_keyboard *kb){if(hw.owner!=kb)uihw_reset(kb);}
static gfx_rect button(int id,int y,bool url)
{
    if(id<=C2)return gfx_r(4+id*44,y+4,40,24);
    if(id==SHIFT)return gfx_r(136,y+4,28,24);
    if(id==NUMBERS)return gfx_r(168,y+4,32,24);
    if(id==HELP)return gfx_r(204,y+4,32,24);
    if(id==KEYS)return gfx_r(6,y+114,44,30);
    if(id==CLEAR)return gfx_r(54,y+114,36,30);
    if(id==SPACE)return gfx_r(94,y+114,url?24:52,30);
    if(id==SLASH)return gfx_r(122,y+114,24,30);
    if(id==BACK)return gfx_r(150,y+114,28,30);
    return gfx_r(182,y+114,52,30);
}
static int hit(const struct ui_keyboard *kb,int x,int y)
{
    for(int i=C0;i<=SLASH;i++){
        if(i==SLASH && !kb->url)continue;
        if(gfx_rect_contains(button(i,0,kb->url),x,y))return i;
    }
    return gfx_rect_contains(gfx_r(6,32,228,76),x,y)?DRAW:-1;
}
static void recognize(const struct ui_keyboard *kb)
{hw.count=ui_ink_recognize(&hw.ink,kb->numbers,hw.choices);}
static void glyph(gfx_surface *s,const struct ui_ink *ink,int x,int y,int scale,uint32_t color)
{
    for(unsigned i=0;i<ink->count;i++){
        const struct ui_ink_point *p=&ink->points[i];
        int px=x+p->x*scale/90,py=y+p->y*scale/90;
        if(i && p->stroke==ink->points[i-1].stroke){
            const struct ui_ink_point *q=&ink->points[i-1];
            gfx_line(s,x+q->x*scale/90,y+q->y*scale/90,px,py,color,255);
        }
    }
}
static void guide(gfx_surface *s,bool numbers,int y)
{
    char seen[26]={0};unsigned at=0;
    for(unsigned i=0;i<uihw_examples();i++){
        struct ui_ink example;char ch=uihw_example(i,&example),base=numbers?'0':'a';
        unsigned count=numbers?10:26;if(ch<base || ch>=base+(int)count || seen[(unsigned)(ch-base)])continue;
        seen[(unsigned)(ch-base)]=1;
        int x=7+(int)(at%9)*25,top=y+33+(int)(at/9)*25;
        char label[2]={(char)(numbers?ch:ch-'a'+'A'),0};
        gfx_text(s,x,top,ui_font(FONT_B8),label,UI_TEXT_DETAIL,255);
        glyph(s,&example,x+8,top,17,0x245F95);at++;
    }
}
void uihw_draw(gfx_surface *s,const struct ui_keyboard *kb,int y)
{
    own(kb);
    gfx_vgradient(s,gfx_r(0,y,UI_W,UI_KB_H),0xB7BDC7,0x7E8696,255);
    gfx_fill(s,gfx_r(0,y,UI_W,1),0x5A606C);
    for(int id=C0;id<=SLASH;id++){
        if(id==SLASH && !kb->url)continue;
        char candidate[2]={0};const char *label;
        if(id<=C2){
            if((unsigned)id<hw.count){candidate[0]=hw.choices[id].ch;if(kb->shift && candidate[0]>='a' && candidate[0]<='z')candidate[0]-='a'-'A';}
            label=candidate;
        }else label=id==SHIFT?"^":id==NUMBERS?(kb->numbers?"abc":"123"):
            id==HELP?"?":id==KEYS?"Keys":id==CLEAR?"Clear":id==BACK?GFX_CH_LARROW:
            id==ENTER?(kb->url?"Go":"return"):id==SLASH?"/":(kb->url?".":"space");
        gfx_rect r=button(id,y,kb->url);
        ui_draw_button(s,r,label,kb->pressed==id || (id==SHIFT && kb->shift));
        if(id==C0 && hw.count){
            gfx_fill(s,gfx_r(r.x+2,r.y+1,r.w-4,1),0x376B9B);
            gfx_fill(s,gfx_r(r.x+2,r.y+r.h-2,r.w-4,1),0x376B9B);
        }
    }
    gfx_rect canvas=gfx_r(6,y+32,228,76);gfx_round_rect(s,canvas,4,0xFFFFFF,255);
    gfx_surface ink=*s;gfx_rect_intersect(s->clip,canvas,&ink.clip);
    if(hw.help){guide(&ink,kb->numbers,y);return;}
    gfx_fill(&ink,gfx_r(canvas.x+4,canvas.y+58,canvas.w-8,1),0xD9E1E8);
    for(unsigned i=0;i<hw.ink.count;i++){
        const struct ui_ink_point *p=&hw.ink.points[i];int x=canvas.x+p->x,py=canvas.y+p->y;
        if(i && p->stroke==hw.ink.points[i-1].stroke){
            const struct ui_ink_point *q=&hw.ink.points[i-1];
            gfx_line(&ink,canvas.x+q->x,canvas.y+q->y,x,py,0x184E86,255);
            gfx_line(&ink,canvas.x+q->x+1,canvas.y+q->y,x+1,py,0x184E86,255);
        }else gfx_circle(&ink,x,py,1,0x184E86,255);
    }
    if(!hw.ink.count || hw.ink.overflow){
        gfx_text_center(&ink,canvas.x,canvas.y+17,canvas.w,ui_font(FONT_B11),hw.ink.overflow?"Too much ink. Tap Clear.":kb->numbers?"Print one digit":"Print one letter",UI_TEXT_DETAIL,255);
        gfx_text_center(&ink,canvas.x,canvas.y+36,canvas.w,ui_font(FONT_R10),"Then tap a choice above.",UI_TEXT_DETAIL,255);
    }else if(!hw.ink.drawing && !hw.count)
        gfx_text_center(&ink,canvas.x,canvas.y+60,canvas.w,ui_font(FONT_B8),"No match. Clear and try again.",0x9B3434,255);
}
int uihw_event(struct ui_keyboard *kb,const struct gz_input_event *e,int y,bool *changed)
{
    own(kb);*changed=false;int local=e->y-y;
    if(e->type==GZ_EV_TOUCH_CANCEL || e->type==GZ_EV_APP_DEACTIVATE){uihw_reset(kb);kb->pressed=-1;*changed=true;return 0;}
    if(e->type==GZ_EV_TOUCH_DOWN){
        if(hw.ink.drawing){ui_ink_cancel(&hw.ink);recognize(kb);}
        int id=hit(kb,e->x,local);kb->pressed=id;hw.gesture=id==DRAW;hw.blocked=false;
        if(id==DRAW && !hw.help){
            ui_ink_begin(&hw.ink,e->x-6,local-32);hw.count=0;
            uisnd_sounded(); /* Ink is silent, including dots and crossbars. */
        }
        else if(id>=0){audio_sound(AUDIO_SND_KEY);uisnd_sounded();}
        *changed=true;return 0;
    }
    if(hw.gesture && (e->type==GZ_EV_TOUCH_MOVE || e->type==GZ_EV_TOUCH_UP)){
        bool inside=gfx_rect_contains(gfx_r(6,32,228,76),e->x,local);
        if(!inside && hw.ink.drawing){ui_ink_cancel(&hw.ink);hw.blocked=true;recognize(kb);}
        if(hw.ink.drawing && !hw.blocked){
            if(e->type==GZ_EV_TOUCH_UP){ui_ink_end(&hw.ink,e->x-6,local-32);recognize(kb);}
            else ui_ink_move(&hw.ink,e->x-6,local-32);
        }
        if(e->type==GZ_EV_TOUCH_UP){hw.gesture=false;kb->pressed=-1;}
        *changed=true;return 0;
    }
    if(e->type==GZ_EV_TOUCH_MOVE){int old=kb->pressed;if(hit(kb,e->x,local)!=old)kb->pressed=-1;*changed=old!=kb->pressed;return 0;}
    if(e->type!=GZ_EV_TOUCH_UP)return 0;
    int id=kb->pressed;kb->pressed=-1;*changed=id!=-1;
    if(id<0 || hit(kb,e->x,local)!=id)return 0;
    if(id==SHIFT){kb->shift=!kb->shift;return 0;}
    if(id==NUMBERS){kb->numbers=!kb->numbers;kb->shift=false;recognize(kb);return 0;}
    if(id==HELP){hw.help=!hw.help;return 0;}
    if(id==KEYS){ui_keyboard_set_method(kb,UI_INPUT_QWERTY);return 0;}
    if(id==CLEAR){uihw_reset(kb);return 0;}
    int ch=0;
    if(id<=C2 && (unsigned)id<hw.count){ch=hw.choices[id].ch;if(kb->shift && ch>='a' && ch<='z')ch-='a'-'A';kb->shift=false;}
    if(id==SPACE)ch=kb->url?'.':' ';
    if(id==SLASH)ch='/';
    if(id==BACK)ch='\b';
    if(id==ENTER){ch='\n';kb->shift=!kb->url;}
    if(ch)uihw_reset(kb);
    return ch;
}
