/* SPDX-License-Identifier: GPL-3.0-or-later
 * Glaze application shell for DoomGeneric. No game data is embedded. */
#include "port.h"
#include "doomgeneric.h"
#include "doomstat.h"
#include "doomkeys.h"
#include "m_menu.h"
#include "g_game.h"
#include "p_saveg.h"
#include "i_system.h"
#include <glaze/ui.h>
#include <glaze/fs.h>
#include <glaze/sys.h>

#define VIDEO_Y 77
#define VIDEO_H 180
#define MAX_FILES 64
static struct fs_dirent entries[MAX_FILES];
static struct ui_list library;
static int count,skip,next_skip,selected;
static bool more, playing, overlay, running;
static char directory[FS_PATH_MAX], notice[128];
static gfx_rect back_button,action_button;
static uint32_t clock_offset, frames;

static void chrome(void)
{
    gfx_fill(&ui.surf,gfx_r(0,0,UI_W,UI_H),0x000000);
    ui_draw_navbar(&ui.surf,"Doom","Menu",&back_button,1);
    action_button=ui_draw_bar_button(&ui.surf,"Pause",true,false,false,1);
    gfx_text_center(&ui.surf,0,274,UI_W,ui_font(FONT_R11),"Arrows: move / turn   OK: fire   Esc: use",0xbbbbbb,255);
}
static void draw_overlay(void)
{
    gfx_fill(&ui.surf,gfx_r(0,0,UI_W,UI_H),0x18202b);
    ui_draw_navbar(&ui.surf,"Paused",NULL,NULL,1);
    ui_draw_button(&ui.surf,gfx_r(12,42,104,32),"Resume",false);
    ui_draw_button(&ui.surf,gfx_r(124,42,104,32),"Doom menu",false);
    ui_draw_button(&ui.surf,gfx_r(12,84,104,32),"Save slot 1",false);
    ui_draw_button(&ui.surf,gfx_r(124,84,104,32),"Load slot 1",false);
    ui_draw_button(&ui.surf,gfx_r(12,126,104,32),"Map",false);
    ui_draw_button(&ui.surf,gfx_r(124,126,104,32),running ? "Run: on" : "Run: off",false);
    gfx_text_center(&ui.surf,0,173,UI_W,ui_font(FONT_R12),"Choose weapon",0xffffff,255);
    for (int i=0;i<7;i++) {
        char label[2]={(char)('1'+i),0};
        ui_draw_button(&ui.surf,gfx_r(8+i*32,195,29,32),label,false);
    }
    ui_draw_button(&ui.surf,gfx_r(55,242,130,32),"Quit Doom",false);
    if (*notice) gfx_text_center(&ui.surf,0,285,UI_W,ui_font(FONT_R10),notice,0xffbbbb,255);
    ui_invalidate_all(); ui_present();
}
static void overlay_action(const struct gz_input_event *e)
{
    if (e->type==GZ_EV_APP_ACTIVATE) { draw_overlay(); return; }
    if (e->type==GZ_EV_KEY_DOWN && (e->code==GZ_KEY_OK || e->code==GZ_KEY_ESC)) overlay=false;
    if (e->type!=GZ_EV_TOUCH_UP) return;
    int x=e->x,y=e->y;
    if (y>=42 && y<74) { if (x>=124) doom_input_pulse(KEY_ESCAPE); overlay=false; }
    else if (y>=84 && y<116) {
        if (x<120 && (gamestate!=GS_LEVEL || demoplayback)) { strlcpy(notice,"Start a game first",sizeof notice); draw_overlay(); return; }
        if (x<120) { G_SaveGame(0,"Glaze save"); puts("doom: save requested"); }
        else {
            char *path=P_SaveGameFile(0); struct fs_stat st;
            if (fs_stat(path,&st)<0) { strlcpy(notice,"Slot 1 is empty",sizeof notice); draw_overlay(); return; }
            G_LoadGame(path); puts("doom: load requested");
        }
        overlay=false;
    } else if (y>=126 && y<158) {
        if (x<120) { doom_input_pulse(KEY_TAB); overlay=false; }
        else { running=!running; draw_overlay(); }
    } else if (y>=195 && y<227 && x>=8 && x<232) {
        doom_input_pulse('1'+(x-8)/32); overlay=false;
    } else if (y>=242 && y<274 && x>=55 && x<185) {
        /* Use Doom's own confirmation, accessible with OK / Esc. */
        doom_input_pulse(KEY_F10); overlay=false;
    }
}
static void game_event(const struct gz_input_event *e)
{
    if (e->type==GZ_EV_APP_DEACTIVATE) {
        doom_input_release(); doom_audio_suspend();
        puts("doom: backgrounded");
        uint32_t before=gz_time_ms();
        struct gz_input_event next;
        /* ui_next_event handles SpringBoard suspension; exclude all of it
         * from Doom's clock so returning never runs a backlog of tics. */
        do { ui_next_event(&next,-1); } while (next.type!=GZ_EV_APP_ACTIVATE);
        if (!overlay) clock_offset+=gz_time_ms()-before;
        if (!overlay) { doom_audio_resume(); chrome(); }
        else draw_overlay();
        puts("doom: foregrounded"); return;
    }
    if (e->type==GZ_EV_TOUCH_CANCEL) { doom_input_release(); return; }
    if (overlay) { overlay_action(e); return; }
    if (e->type==GZ_EV_KEY_DOWN || e->type==GZ_EV_KEY_UP) {
        int in_menu=menuactive || gamestate!=GS_LEVEL || demoplayback;
        doom_input_key(e->code,e->type==GZ_EV_KEY_DOWN,in_menu);
    } else if (e->type==GZ_EV_TEXT && e->code>=32 && e->code<127) {
        unsigned c=e->code;
        if (!menuactive) {
            if (c==' ') c=KEY_USE;
            else if (c=='z') c=KEY_STRAFE_L;
            else if (c=='x') c=KEY_STRAFE_R;
            else if (c=='p') c=KEY_PAUSE;
        }
        doom_input_text(c);
    } else if (e->type==GZ_EV_TOUCH_UP) {
        if (gfx_rect_contains(back_button,e->x,e->y)) doom_input_pulse(KEY_ESCAPE);
        else if (gfx_rect_contains(action_button,e->x,e->y)) {
            doom_input_release(); overlay=true; notice[0]=0; draw_overlay();
        }
    }
}
void doom_pump(void)
{
    if (!playing) return;
    struct gz_input_event e;
    while (ui_next_event(&e,0)) game_event(&e);
    if (overlay) {
        uint32_t before=gz_time_ms();
        doom_audio_suspend(); puts("doom: paused");
        while (overlay) if (ui_next_event(&e,100)) game_event(&e);
        clock_offset+=gz_time_ms()-before;
        doom_audio_resume(); chrome(); puts("doom: resumed");
    }
}
int DG_GetKey(int *pressed,unsigned char *key)
{
    doom_input_run(running);
    return doom_input_next(pressed,key);
}
void DG_Init(void) {}
void DG_DrawFrame(void) {}
void DG_SetWindowTitle(const char *s) { printf("doom: %s\n",s); }
uint32_t DG_GetTicksMs(void) { return gz_time_ms()-clock_offset; }
void DG_SleepMs(uint32_t ms)
{
    doom_pump(); if (ms>10) ms=10; if (ms) gz_sleep_ms(ms);
}
void doom_present(const uint8_t *pixels,const uint16_t *palette)
{
    for (int y=0;y<VIDEO_H;y++) {
        uint16_t *out=ui.surf.px+(VIDEO_Y+y)*ui.surf.stride;
        const uint8_t *in=pixels+(y*200/VIDEO_H)*320;
        for (int x=0;x<UI_W;x++) out[x]=palette[in[x*4/3]];
    }
    ui_invalidate_all(); ui_present();
    if (++frames==1 || frames%350==0) printf("doom: frame %u\n",frames);
}
void doom_failure(const char *msg)
{
    gfx_rect ok,other;
    ui_draw_alert(&ui.surf,"Doom stopped",msg,"Close",NULL,&ok,&other,-1);
    ui_invalidate_all(); ui_present();
    struct gz_input_event e;
    for (;;) if (ui_next_event(&e,-1) && (e.type==GZ_EV_TOUCH_UP || e.type==GZ_EV_KEY_DOWN)) exit(1);
}
static int compare(const void *a,const void *b)
{
    const struct fs_dirent *x=a,*y=b;
    if (x->type!=y->type) return x->type==FS_DIR ? -1 : 1;
    return strcasecmp(x->name,y->name);
}
static void browse(void)
{
    count=0; more=false; selected=0; notice[0]=0;
    if (!*directory) {
        const char *roots[]={"Documents","Card"};
        for (int i=0;i<2;i++) { memset(&entries[i],0,sizeof entries[i]); strlcpy(entries[i].name,roots[i],FS_NAME_MAX); entries[i].type=FS_DIR; }
        count=2;
    } else {
        int pos=skip;
        for (int scanned=0;scanned<MAX_FILES;) {
            struct fs_dirent batch[8]; int n=fs_list(directory,pos,batch,8);
            if (n<0) { strlcpy(notice,fs_strerror(n),sizeof notice); break; }
            if (!n) break;
            for (int i=0;i<n;i++) {
                batch[i].name[FS_NAME_MAX-1]=0;
                const char *dot=strrchr(batch[i].name,'.');
                if (!strcmp(batch[i].name,".") || !strcmp(batch[i].name,"..")) continue;
                if (batch[i].type==FS_DIR || (dot && !strcasecmp(dot,".wad"))) entries[count++]=batch[i];
            }
            pos+=n; scanned+=n;
            if (n<8) break;
            if (scanned==MAX_FILES) more=true;
        }
        next_skip=pos; qsort(entries,count,sizeof entries[0],compare);
    }
    ui_list_init(&library,gfx_r(0,30,UI_W,220),34); ui_list_reset(&library,count);
}
static void row(gfx_surface *s,int i,gfx_rect r,bool pressed,void *ctx)
{
    (void)ctx; ui_draw_list_row(s,r,pressed || i==selected);
    gfx_text(s,r.x+10,r.y+9,ui_font(FONT_R13),entries[i].name,UI_TEXT,255);
    if (entries[i].type==FS_DIR) ui_draw_chevron(s,r.x+r.w-15,r.y+17,UI_TEXT_DETAIL);
}
static void draw_library(void)
{
    gfx_fill(&ui.surf,gfx_r(0,0,UI_W,UI_H),0xe8ecf0);
    ui_draw_navbar(&ui.surf,*directory ? strrchr(directory,'/')+1 : "Doom",*directory ? "Back" : NULL,&back_button,0);
    ui_list_draw(&ui.surf,&library,row,NULL);
    gfx_text_center(&ui.surf,0,254,UI_W,ui_font(FONT_R10),*notice ? notice : "Select a Doom / Doom II IWAD. No data included.",UI_TEXT_DETAIL,255);
    if (more) ui_draw_button(&ui.surf,gfx_r(130,277,100,24),"More...",false);
    if (skip) ui_draw_button(&ui.surf,gfx_r(10,277,100,24),"First page",false);
    ui_invalidate_all(); ui_present();
}
static void choose(int i)
{
    if (i<0 || i>=count) return;
    char path[FS_PATH_MAX];
    if (snprintf(path,sizeof path,"%s/%s",directory,entries[i].name)>=(int)sizeof path) { strlcpy(notice,"Path is too long",sizeof notice); return; }
    if (entries[i].type==FS_DIR) { strlcpy(directory,path,sizeof directory); skip=0; browse(); return; }
    uint32_t id;
    const char *error=doom_check_wad(path,&id);
    if (error) { strlcpy(notice,error,sizeof notice); printf("doom: %s\n",error); return; }
    if (snprintf(doom_data_dir,sizeof doom_data_dir,"%s/data/%08x",res_bundle(),id)>=(int)sizeof doom_data_dir || fs_mkdirs(doom_data_dir)<0) {
        strlcpy(notice,"Cannot create save folder",sizeof notice); return;
    }
    printf("doom: loading %s\n",path);
    playing=true; chrome();
    gfx_text_center(&ui.surf,0,140,UI_W,ui_font(FONT_R14),"Loading WAD...",0xffffff,255);
    ui_invalidate_all(); ui_present();
    char *args[]={"doom","-iwad",path,"-nomusic","-nogui",NULL};
    doomgeneric_Create(5,args);
    for (;;) doomgeneric_Tick();
}
int main(void)
{
    ui_init(SB_STATUS_BLACK); browse(); draw_library(); puts("doom: ready");
    for (;;) {
        struct gz_input_event e; bool dirty=false;
        if (ui_next_event(&e,25)) {
            if (e.type==GZ_EV_APP_ACTIVATE) dirty=true;
            else if ((e.type==GZ_EV_TOUCH_UP && *directory && gfx_rect_contains(back_button,e.x,e.y)) || (e.type==GZ_EV_KEY_DOWN && e.code==GZ_KEY_ESC && *directory)) {
                char *slash=strrchr(directory,'/'); if (slash==directory || !slash) directory[0]=0; else *slash=0;
                skip=0; browse(); dirty=true;
            } else if (e.type==GZ_EV_TOUCH_UP && more && gfx_rect_contains(gfx_r(130,277,100,24),e.x,e.y)) {
                skip=next_skip; browse(); dirty=true;
            } else if (e.type==GZ_EV_TOUCH_UP && skip && gfx_rect_contains(gfx_r(10,277,100,24),e.x,e.y)) {
                skip=0; browse(); dirty=true;
            } else if (e.type==GZ_EV_KEY_DOWN) {
                if (e.code==GZ_KEY_UP && selected>0) selected--;
                else if (e.code==GZ_KEY_DOWN && selected<count-1) selected++;
                else if (e.code==GZ_KEY_OK || e.code==GZ_KEY_ENTER) choose(selected);
                int top=selected*34;
                if (top<library.sc.offset) library.sc.offset=top;
                if (top+34>library.sc.offset+library.area.h) library.sc.offset=top+34-library.area.h;
                dirty=true;
            } else { int i=ui_list_event(&library,&e,&dirty); if (i>=0) { selected=i; choose(i); dirty=true; } }
        }
        if (ui_scroll_step(&library.sc)) dirty=true;
        if (dirty) draw_library();
    }
}
