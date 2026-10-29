/* Independent stylus traces plus geometry, bounds, ABI and real panel events. */
#include <glaze/ui.h>
#include <glaze/handwriting.h>
#include <glaze/prefs.h>
#include "../lib/ui/ink_internal.h"
#include "hostres.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct ui_app ui;
static uint32_t preference;
static int pref_size=-1,pref_error,sounds;
int pref_get(const char *key,void *out,uint32_t size)
{assert(!strcmp(key,UI_INPUT_PREF));if(pref_size>0)memcpy(out,&preference,MIN(size,sizeof(preference)));return pref_size;}
int pref_put(const char *key,const void *value,uint32_t size)
{assert(!strcmp(key,UI_INPUT_PREF));if(pref_error)return pref_error;memcpy(&preference,value,4);pref_size=size;return 0;}
void audio_sound(int id){sounds++;}
void uisnd_sounded(void){}
uint32_t gz_time_ms(void){return 0;}
struct p {int x,y;};
static void stroke(struct ui_ink *ink,const struct p *points,unsigned count)
{
    assert(count && ui_ink_begin(ink,points[0].x,points[0].y));
    for(unsigned i=1;i<count;i++)ui_ink_move(ink,points[i].x,points[i].y);
    ui_ink_end(ink,points[count-1].x,points[count-1].y);
}
static void expect(struct ui_ink *ink,bool numbers,char letter)
{
    struct ui_ink_choice c[3];unsigned n=ui_ink_recognize(ink,numbers,c);
    if(!n || c[0].ch!=letter){fprintf(stderr,"expected %c, got",letter);for(unsigned i=0;i<n;i++)fprintf(stderr," %c:%u",c[i].ch,c[i].distance);fputc('\n',stderr);abort();}
}
static struct ui_ink traces(void)
{
    struct ui_ink ink;
    ui_ink_clear(&ink);
    const struct p a[]={{20,80},{29,60},{40,38},{53,11},{64,34},{74,58},{85,80}};
    const struct p bar[]={{33,58},{44,58},{56,58},{72,58}};
    stroke(&ink,a,ARRAY_SIZE(a));stroke(&ink,bar,ARRAY_SIZE(bar));expect(&ink,false,'a');
    struct ui_ink original=ink;
    ui_ink_clear(&ink);const struct p l[]={{20,10},{20,45},{20,75},{45,75},{70,75}};
    stroke(&ink,l,ARRAY_SIZE(l));expect(&ink,false,'l');
    ui_ink_clear(&ink);const struct p c[]={{76,16},{52,8},{30,18},{18,42},{23,65},{50,78},{76,70}};
    stroke(&ink,c,ARRAY_SIZE(c));expect(&ink,false,'c');
    ui_ink_clear(&ink);const struct p four[]={{68,8},{46,33},{22,58},{53,58},{88,58}},stem[]={{68,8},{68,45},{68,88}};
    stroke(&ink,four,ARRAY_SIZE(four));stroke(&ink,stem,ARRAY_SIZE(stem));expect(&ink,true,'4');
    ui_ink_clear(&ink);const struct p two[]={{15,20},{30,10},{55,12},{65,25},{60,38},{15,80},{70,80}};
    stroke(&ink,two,ARRAY_SIZE(two));expect(&ink,true,'2');
    ui_ink_clear(&ink);const struct p eight_top[]={{50,10},{30,14},{25,30},{45,43},{65,36},{70,20},{50,10}};
    const struct p eight_bottom[]={{45,43},{25,57},{30,77},{50,85},{73,75},{75,58},{45,43}};
    stroke(&ink,eight_top,ARRAY_SIZE(eight_top));stroke(&ink,eight_bottom,ARRAY_SIZE(eight_bottom));expect(&ink,true,'8');
    ui_ink_clear(&ink);const struct p i[]={{50,35},{50,57},{50,80}},dot[]={{50,14}};
    stroke(&ink,i,ARRAY_SIZE(i));stroke(&ink,dot,1);expect(&ink,false,'i');
    return original;
}
static void geometry(const struct ui_ink *original)
{
    for(unsigned scale=1;scale<=4;scale++){
        struct ui_ink transformed=*original;
        for(unsigned i=0;i<transformed.count;i++){
            transformed.points[i].x=20+transformed.points[i].x*scale;
            transformed.points[i].y=30+transformed.points[i].y*scale;
        }
        expect(&transformed,false,'a');
    }
    struct ui_ink reverse;ui_ink_clear(&reverse);
    for(int s=original->strokes-1;s>=0;s--){
        bool first=true;
        for(int i=original->count-1;i>=0;i--)if(original->points[i].stroke==s){
            int x=original->points[i].x,y=original->points[i].y;
            if(first){ui_ink_begin(&reverse,x,y);first=false;}else ui_ink_move(&reverse,x,y);
        }
        reverse.drawing=false;
    }
    expect(&reverse,false,'a');
    unsigned covered[36]={0};
    for(unsigned i=0;i<uihw_examples();i++){
        struct ui_ink example;char ch=uihw_example(i,&example);bool numbers=ch<='9';
        struct ui_ink_choice choices[3];unsigned n=ui_ink_recognize(&example,numbers,choices);bool found=false;
        for(unsigned k=0;k<n;k++)if(choices[k].ch==ch)found=true;
        assert(found);covered[numbers?ch-'0':10+ch-'a']++;
    }
    for(unsigned i=0;i<36;i++)assert(covered[i]);
}
static void limits(void)
{
    struct ui_ink ink;struct ui_ink_choice out[3];ui_ink_clear(&ink);
    assert(!ui_ink_recognize(&ink,false,out));assert(!ui_ink_begin(&ink,-1,0));assert(!ui_ink_begin(&ink,1024,0));
    assert(ui_ink_begin(&ink,10,10));ui_ink_end(&ink,10,10);assert(!ui_ink_recognize(&ink,false,out));
    ui_ink_clear(&ink);assert(ui_ink_begin(&ink,10,10));ui_ink_move(&ink,50,50);
    assert(!ui_ink_recognize(&ink,false,out));ui_ink_cancel(&ink);assert(!ink.count && !ink.strokes);
    for(unsigned i=0;i<UI_INK_STROKES;i++){assert(ui_ink_begin(&ink,10,10));ui_ink_end(&ink,50,50);}
    assert(!ui_ink_begin(&ink,10,10) && ink.overflow && !ui_ink_recognize(&ink,false,out));
    ui_ink_clear(&ink);assert(ui_ink_begin(&ink,0,0));
    for(unsigned i=1;i<=UI_INK_POINTS;i++)ui_ink_move(&ink,i%100,(i*7)%100);
    ui_ink_end(&ink,0,0);assert(ink.overflow && !ui_ink_recognize(&ink,false,out));
    ink.count=65535;assert(!ui_ink_recognize(&ink,false,out));
}
static int event(struct ui_keyboard *kb,unsigned type,int x,int y)
{
    struct gz_input_event e={.type=type,.x=x,.y=UI_H-UI_KB_H+y};bool changed;
    return ui_keyboard_event(kb,&e,UI_H-UI_KB_H,&changed);
}
static int tap(struct ui_keyboard *kb,int x,int y)
{assert(!event(kb,GZ_EV_TOUCH_DOWN,x,y));return event(kb,GZ_EV_TOUCH_UP,x,y);}
static void write_a(struct ui_keyboard *kb)
{
    event(kb,GZ_EV_TOUCH_DOWN,66,98);event(kb,GZ_EV_TOUCH_MOVE,96,38);event(kb,GZ_EV_TOUCH_UP,126,98);
    event(kb,GZ_EV_TOUCH_DOWN,79,76);event(kb,GZ_EV_TOUCH_UP,113,76);
}
static void panel(void)
{
    struct {struct ui_keyboard kb;uint32_t guard;} boxed={.guard=0x1234abcd};
    struct ui_keyboard *kb=&boxed.kb;
    assert(ui_input_method_default()==UI_INPUT_QWERTY);
    preference=1;pref_size=5;assert(ui_input_method_default()==UI_INPUT_QWERTY);pref_size=-1;
    ui_keyboard_init(kb);assert(boxed.guard==0x1234abcd && sizeof(*kb)==8 && kb->method==UI_INPUT_QWERTY);
    assert(!tap(kb,70,128) && kb->method==UI_INPUT_HANDWRITING);
    write_a(kb);
    uint16_t pixels[UI_W*UI_H];gfx_surface s;gfx_surface_init(&s,pixels,UI_W,UI_H,UI_W);gfx_fill(&s,gfx_r(0,0,UI_W,UI_H),0xF9F5DC);
    ui_draw_navbar(&s,"Handwriting",NULL,NULL,0);gfx_text_center(&s,0,70,UI_W,ui_font(FONT_R17),"Write, then choose a letter",UI_TEXT,255);
    ui_draw_keyboard(&s,kb,UI_H-UI_KB_H);host_write_bmp("../out/host/handwriting.bmp",&s);
    assert(tap(kb,20,15)=='A');
    event(kb,GZ_EV_TOUCH_DOWN,66,38);event(kb,GZ_EV_TOUCH_MOVE,66,98);event(kb,GZ_EV_TOUCH_UP,116,98);
    assert(tap(kb,20,15)=='l');
    write_a(kb);event(kb,GZ_EV_TOUCH_CANCEL,0,0);assert(tap(kb,20,15)==0);
    assert(tap(kb,160,128)=='\b');assert(tap(kb,120,128)==' ');assert(tap(kb,205,128)=='\n' && kb->shift);
    tap(kb,220,15);ui_draw_keyboard(&s,kb,UI_H-UI_KB_H);host_write_bmp("../out/host/handwriting-help.bmp",&s);tap(kb,220,15);
    tap(kb,20,128);assert(kb->method==UI_INPUT_QWERTY);
    kb->url=true;assert(!tap(kb,70,128));
    assert(tap(kb,106,128)=='.');assert(tap(kb,134,128)=='/');assert(tap(kb,160,128)=='\b');assert(tap(kb,205,128)=='\n');
    tap(kb,20,128);assert(tap(kb,164,128)==UI_KEY_DOTCOM);
    assert(!ui_input_method_save(UI_INPUT_HANDWRITING));ui_keyboard_init(kb);assert(kb->method==UI_INPUT_HANDWRITING);
    assert(ui_input_method_save(99)==GZ_EINVAL);pref_error=GZ_EIO;assert(ui_input_method_save(UI_INPUT_QWERTY)==GZ_EIO);pref_error=0;
    ui.hardware_keyboard=true;assert(ui_keyboard_height()==0);ui.hardware_keyboard=false;assert(ui_keyboard_height()==150);
    assert(sounds>0);
}
int main(void)
{
    struct ui_ink original=traces();geometry(&original);limits();panel();
    puts("handwriting: independent letter/digit traces, transforms, all glyphs, bounds, ABI, panel, URL and defaults PASS");return 0;
}
