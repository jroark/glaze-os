/* Shared Sega app: ROM library, portrait display, controls and eager battery saves. */
#include "core.h"
#include "save.h"
#include <glaze/audio.h>
#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LIST_MAX 64
#ifdef SEGA_GENESIS
#define SEGA_TITLE "Genesis"
#define SEGA_EXTENSION ".gen"
#define SEGA_DEMO "demo.gen"
#define SEGA_START_NAME "Start"
#elif defined(SEGA_GAMEGEAR)
#define SEGA_TITLE "Game Gear"
#define SEGA_EXTENSION ".gg"
#define SEGA_DEMO "demo.gg"
#define SEGA_START_NAME "Start"
#elif defined(SEGA_TURBOGRAFX)
#define SEGA_TITLE "TurboGrafx"
#define SEGA_EXTENSION ".pce"
#define SEGA_DEMO "demo.pce"
#define SEGA_START_NAME "Run"
#else
#define SEGA_TITLE "Master System"
#define SEGA_EXTENSION ".sms"
#define SEGA_DEMO "demo.sms"
#define SEGA_START_NAME "Console Pause"
#endif
#ifndef SEGA_TURBOGRAFX
static unsigned button_pair; /* Genesis: B/C, A/B or A/C. */
static unsigned missing_button(void) {
    static const unsigned missing[]={SEGA_A,SEGA_C,SEGA_B};
    return missing[button_pair];
}
#endif
#ifdef SEGA_GENESIS
static const char *button_label(unsigned b) { return b==SEGA_A ? "A" : b==SEGA_B ? "B" : "C"; }
#endif

static struct fs_dirent entries[LIST_MAX];
static struct ui_list list;
static int entry_count, page_skip, next_skip;
static bool more, paused, reset_requested, sound_enabled = true, stream_open;
static bool exit_game, save_failed;
static char directory[FS_PATH_MAX], notice[96], title[FS_NAME_MAX], save_path[FS_PATH_MAX];
static gfx_rect back_button, pause_button;
static uint8_t keys, pulse_keys;
static uint32_t saved_hash, save_time, frame_count, start_time;
static uint64_t frame_deadline;
static uint32_t frame_fraction;
static unsigned rate=22050;
static void timing_reset(void) { frame_deadline=gz_time_us(); frame_fraction=0; }


void sega_log(const char *s) { printf("sega: %s\n", s); }
void sega_audio_close(void)
{
    if (stream_open) audio_stream_close();
    stream_open = false;
}
void sega_audio_open(unsigned hz)
{
    rate = hz;
    if (stream_open) return;
    if (sound_enabled) stream_open = audio_stream_open(hz,1) >= 0;
}
void sega_audio(const int16_t *pcm, unsigned samples)
{
    if (!stream_open || paused) return;
    /* Bound buffering to keep controls responsive if audio stalls/takes over. */
    uint32_t queued, played;
    if (audio_stream_clock(&played,&queued) >= 0 && queued > 100) return;
    int n = audio_stream_write(pcm, samples * 2);
    if (n < 0) { sega_audio_close(); return; }
    if (n > 0) audio_stream_write(NULL,0);
}

static void save_battery(void)
{
    if (!sega_cartridge.battery || !sega_cartridge.ram_size || !*save_path) return;
    uint32_t hash=sega_hash(sega_ram(),sega_cartridge.ram_size);
    if (hash==saved_hash) return;
    char folder[FS_PATH_MAX];
    snprintf(folder,sizeof folder,"%s/data",res_bundle());
    int rv=fs_mkdirs(folder);
    if (rv>=0) rv=sega_save_write(save_path,sega_ram(),sega_cartridge.ram_size);
    if (rv>=0) { saved_hash=hash; puts("sega: battery saved"); }
    else if(!save_failed) puts("sega: battery save failed");
    save_failed=rv<0;
}

static void draw_game(void)
{
    gfx_fill(&ui.surf,gfx_r(0,UI_NAV_H,UI_W,UI_H-UI_NAV_H),0x000000);
    ui_draw_navbar(&ui.surf,paused ? "Paused" : SEGA_TITLE,"Library",&back_button,1);
    pause_button=ui_draw_bar_button(&ui.surf,paused ? "Play" : "Pause",true,false,true,1);
    if (paused) {
        gfx_fill_alpha(&ui.surf,gfx_r(0,UI_NAV_H,UI_W,UI_H-UI_NAV_H),0x101827,230);
        gfx_text_center(&ui.surf,0,65,240,ui_font(FONT_B17),title,0xffffff,255);
        ui_draw_button(&ui.surf,gfx_r(65,96,110,30),"Reset",false);
        ui_draw_button(&ui.surf,gfx_r(65,136,110,30),sound_enabled ? "Sound On" : "Sound Off",false);
#if defined(SEGA_GENESIS) || defined(SEGA_TURBOGRAFX)
#ifdef SEGA_GENESIS
        ui_draw_button(&ui.surf,gfx_r(15,176,100,30),"Start",false);
        ui_draw_button(&ui.surf,gfx_r(125,176,100,30),button_label(missing_button()),false);
        static const char *pairs[]={"OK/Esc: B/C","OK/Esc: A/B","OK/Esc: A/C"};
        ui_draw_button(&ui.surf,gfx_r(10,216,120,30),pairs[button_pair],false);
#else
        ui_draw_button(&ui.surf,gfx_r(15,176,100,30),"Run",false);
        ui_draw_button(&ui.surf,gfx_r(125,176,100,30),"Select",false);
#endif
#else
        ui_draw_button(&ui.surf,gfx_r(40,176,160,30),SEGA_START_NAME,false);
#endif
#if !defined(SEGA_GAMEGEAR) && !defined(SEGA_TURBOGRAFX)
        ui_draw_button(&ui.surf,gfx_r(135,216,95,30),sega_cartridge.pal ? "PAL" : "NTSC",false);
#endif
        if (save_failed) gfx_text_center(&ui.surf,0,264,240,ui_font(FONT_R11),"Could not save battery RAM",0xffbbbb,255);
    }
    ui_invalidate_all(); ui_present();
}

void sega_video(const uint16_t *pixels)
{
    if (paused) return;
    /* Preserve the full picture and aspect ratio, centred below navigation. */
    unsigned w=sega_width(),h=sega_height();
    if(!w || !h || w>512 || h>240) return;
    unsigned vh=h*UI_W/w;
#ifdef SEGA_TURBOGRAFX
    vh=180; /* Dot-clock-dependent pixel widths all target a 4:3 television. */
#endif
    unsigned top=UI_NAV_H+(UI_H-UI_NAV_H-vh)/2;
    for (unsigned y=0;y<vh;y++) {
        uint16_t *out=ui.surf.px+(y+top)*ui.surf.stride;
        const uint16_t *in=pixels+(y*h/vh)*w;
        for (unsigned x=0;x<UI_W;x++) out[x]=in[x*w/UI_W];
    }
    ui_invalidate(gfx_r(0,top,UI_W,vh));
    if (save_failed) {
        gfx_text_center(&ui.surf,0,286,240,ui_font(FONT_R10),"Could not save battery data",0xffbbbb,255);
        ui_invalidate(gfx_r(0,280,UI_W,24));
    }
    ui_present();
}

static uint8_t key_bit(unsigned code)
{
    switch(code) {
    case GZ_KEY_UP:return SEGA_UP; case GZ_KEY_DOWN:return SEGA_DOWN;
    case GZ_KEY_LEFT:return SEGA_LEFT; case GZ_KEY_RIGHT:return SEGA_RIGHT;
    case GZ_KEY_OK:
#ifdef SEGA_GENESIS
        return button_pair==0 ? SEGA_B : SEGA_A;
#else
        return SEGA_A;
#endif
    case GZ_KEY_ESC:
#ifdef SEGA_GENESIS
        return button_pair==1 ? SEGA_B : SEGA_C;
#else
        return SEGA_B;
#endif
    case GZ_KEY_ENTER:return SEGA_START;
    case GZ_KEY_TAB:
#ifdef SEGA_TURBOGRAFX
        return SEGA_C;
#else
        return missing_button();
#endif
    default:return 0;
    }
}

static void resume_game(void)
{
    paused=false;
    sega_audio_open(rate);
    timing_reset();
    puts("sega: resumed");
    draw_game();
}

static void game_event(const struct gz_input_event *e)
{
    if (e->type==GZ_EV_APP_DEACTIVATE) {
        keys=pulse_keys=0;
        save_battery();
        sega_audio_close();
        puts("sega: backgrounded");
    } else if (e->type==GZ_EV_APP_ACTIVATE) {
        timing_reset();
        if (!paused) sega_audio_open(rate);
        draw_game();
        puts("sega: foregrounded");
    } else if (e->type==GZ_EV_TOUCH_CANCEL) {
        keys=pulse_keys=0;
    } else if (e->type==GZ_EV_KEY_DOWN || e->type==GZ_EV_KEY_UP) {
        uint8_t b=key_bit(e->code);
        if (e->type==GZ_EV_KEY_DOWN) { if (b) printf("sega: input %u\n",(unsigned)b); keys|=b; pulse_keys|=b; } else keys&=~b;
    } else if (e->type==GZ_EV_TOUCH_UP) {
        if (gfx_rect_contains(back_button,e->x,e->y)) { exit_game=true; save_battery(); }
        else if (gfx_rect_contains(pause_button,e->x,e->y)) {
            keys=pulse_keys=0;
            if (!paused) {
                paused=true; save_battery(); sega_audio_close(); puts("sega: paused");
                draw_game();
            } else resume_game();
        } else if (paused && gfx_rect_contains(gfx_r(65,96,110,30),e->x,e->y)) {
            save_battery(); reset_requested=true; exit_game=true;
        } else if (paused && gfx_rect_contains(gfx_r(65,136,110,30),e->x,e->y)) {
            sound_enabled=!sound_enabled; puts(sound_enabled ? "sega: sound on" : "sega: sound off"); draw_game();
        } else if (paused && gfx_rect_contains(
#if defined(SEGA_GENESIS) || defined(SEGA_TURBOGRAFX)
            gfx_r(15,176,100,30),
#else
            gfx_r(40,176,160,30),
#endif
            e->x,e->y)) {
            pulse_keys|=SEGA_START; puts("sega: console Start/Pause"); resume_game();
#if defined(SEGA_GENESIS) || defined(SEGA_TURBOGRAFX)
        } else if (paused && gfx_rect_contains(gfx_r(125,176,100,30),e->x,e->y)) {
#ifdef SEGA_TURBOGRAFX
            pulse_keys|=SEGA_C;
#else
            pulse_keys|=missing_button();
#endif
            puts("sega: extra button"); resume_game();
#ifdef SEGA_GENESIS
        } else if (paused && gfx_rect_contains(gfx_r(10,216,120,30),e->x,e->y)) {
            button_pair=(button_pair+1)%3; keys=pulse_keys=0; puts("sega: remapped buttons"); draw_game();
#endif
#endif
#if !defined(SEGA_GAMEGEAR) && !defined(SEGA_TURBOGRAFX)
        } else if (paused && gfx_rect_contains(gfx_r(135,216,95,30),e->x,e->y)) {
            save_battery(); sega_set_pal(!sega_cartridge.pal); keys=pulse_keys=0;
            timing_reset(); puts("sega: changed TV standard"); draw_game();
#endif
        }
    }
}

static unsigned sega_poll(void)
{
poll_again:;
    struct gz_input_event e;
    while (ui_next_event(&e,0)) game_event(&e);
    while (paused && !exit_game) {
        if (ui_next_event(&e,100)) game_event(&e);
    }
    if (exit_game) return 256;
    uint32_t now=gz_time_ms();
    if ((uint32_t)(now-save_time)>=5000) { save_battery(); save_time=now; }
    frame_count++;
    unsigned fps=sega_cartridge.pal ? 50 : 60;
    frame_fraction += 1000000u%fps;
    frame_deadline += 1000000u/fps + frame_fraction/fps;
    frame_fraction %= fps;
    int64_t delta=(int64_t)(frame_deadline-gz_time_us());
    if (delta>0) {
        int wait=(int)((delta+999)/1000);
        if (wait>20) wait=20;
        while (wait>0 && ui_next_event(&e,wait)) {
            game_event(&e);
            if (exit_game || paused) break;
            delta=(int64_t)(frame_deadline-gz_time_us());
            wait=delta>0 ? (int)((delta+999)/1000) : 0;
        }
    } else if (delta< -100000) timing_reset();
    if (paused) goto poll_again;
    if (frame_count % 300 == 0) {
        uint32_t elapsed=gz_time_ms()-start_time;
        printf("sega: %u frames in %u ms (%u fps)\n",frame_count,elapsed,elapsed ? frame_count*1000/elapsed : 0);
    }
    unsigned result=exit_game ? 256 : keys|pulse_keys;
    pulse_keys=0;
    return result;
}

static int entry_compare(const void *a,const void *b)
{
    const struct fs_dirent *x=a,*y=b;
    if (x->type!=y->type) return x->type==FS_DIR ? -1 : 1;
    return strcasecmp(x->name,y->name);
}

static void browse(void)
{
    entry_count=0; more=false; notice[0]=0;
    if (!*directory) {
        const char *names[]={"Glaze demo","Documents","Card"};
        for (int i=0;i<3;i++) { memset(&entries[i],0,sizeof entries[i]); strlcpy(entries[i].name,names[i],FS_NAME_MAX); }
        entry_count=3;
    } else {
        struct fs_dirent batch[8];
        int skip=page_skip;
        for (int scanned=0;scanned<LIST_MAX;) {
            int n=fs_list(directory,skip,batch,8);
            if (n<0) { snprintf(notice,sizeof notice,"%s",fs_strerror(n)); break; }
            if (!n) break;
            for (int i=0;i<n;i++) {
                batch[i].name[FS_NAME_MAX-1]=0;
                const char *dot=strrchr(batch[i].name,'.');
                if (!strcmp(batch[i].name,".") || !strcmp(batch[i].name,"..")) continue;
                if (batch[i].type==FS_DIR || (dot && (!strcasecmp(dot,SEGA_EXTENSION)
#ifdef SEGA_GENESIS
                || !strcasecmp(dot,".md") || !strcasecmp(dot,".bin")
#endif
                ))) entries[entry_count++]=batch[i];
            }
            skip+=n; scanned+=n;
            if (n<8) break;
            if (scanned==LIST_MAX) more=true;
        }
        next_skip=skip;
        qsort(entries,entry_count,sizeof entries[0],entry_compare);
    }
    if (!entry_count && !more && !*notice) strlcpy(notice,"No supported ROMs in this folder",sizeof notice);
    ui_list_init(&list,gfx_r(0,30,240,230),34);
    ui_list_reset(&list,entry_count);
}

static void draw_row(gfx_surface *s,int i,gfx_rect r,bool selected,void *ctx)
{
    (void)ctx; ui_draw_list_row(s,r,selected);
    gfx_text(s,r.x+10,r.y+9,ui_font(FONT_R13),entries[i].name,UI_TEXT,255);
    if (entries[i].type==FS_DIR || (!*directory && i>=1)) ui_draw_chevron(s,r.x+r.w-15,r.y+17,UI_TEXT_DETAIL);
}

static void draw_library(void)
{
    gfx_fill_alpha(&ui.surf,gfx_r(0,0,240,304),0xe8ecf0,255);
    ui_draw_navbar(&ui.surf,*directory ? strrchr(directory,'/')+1 : SEGA_TITLE,*directory ? "Back" : NULL,&back_button,0);
    ui_list_draw(&ui.surf,&list,draw_row,NULL);
    const gfx_font *f=ui_font(FONT_R10);
    gfx_text_center(&ui.surf,0,263,240,f,*notice ? notice : "Arrows + OK/Esc. Enter: " SEGA_START_NAME,UI_TEXT_DETAIL,255);
    if (more) ui_draw_button(&ui.surf,gfx_r(130,278,100,24),"More...",false);
    if (page_skip) ui_draw_button(&ui.surf,gfx_r(10,278,100,24),"First page",false);
    if (!*directory) gfx_text_center(&ui.surf,0,284,240,f,"Enter: " SEGA_START_NAME,UI_TEXT_DETAIL,255);
    ui_invalidate_all(); ui_present();
}

static void play(const char *path,int demo)
{
    uint8_t *owned=NULL;
    const uint8_t *data;
    uint32_t size;
    if (demo) data=res_map(SEGA_DEMO,&size);
    else {
        struct fs_stat st;
        if (fs_stat(path,&st)<0 || st.type!=FS_FILE || st.size>SEGA_ROM_MAX) {
            strlcpy(notice,"Cannot open ROM (maximum 4 MiB)",sizeof notice); return;
        }
        size=st.size; owned=malloc(size); data=owned;
        int fd=owned ? fs_open(path,FS_O_READ) : -1;
        uint32_t done=0;
        if (fd>=0) {
            while (done<size) {
                int n=fs_read(fd,owned+done,size-done);
                if (n<=0) break;
                done+=(unsigned)n;
            }
            fs_close(fd);
        }
        if (done!=size || !owned) { free(owned); strlcpy(notice,"Could not read ROM",sizeof notice); return; }
    }
    if (!data) { strlcpy(notice,"Demo resource is missing",sizeof notice); return; }
    const char *error=sega_open(data,size);
    if (error) { free(owned); strlcpy(notice,error,sizeof notice); return; }
    if (snprintf(save_path,sizeof save_path,"%s/data/%08x.sav",res_bundle(),sega_cartridge.hash)>=(int)sizeof save_path) {
        sega_close(); free(owned); strlcpy(notice,"App bundle path is too long",sizeof notice); return;
    }
    if (sega_cartridge.battery && sega_cartridge.ram_size) {
        uint8_t *saved=malloc(sega_cartridge.ram_size);
        int n=saved ? sega_save_read(save_path,saved,sega_cartridge.ram_size) : GZ_ENOMEM;
        if (n==(int)sega_cartridge.ram_size) {
            memcpy(sega_ram(),saved,sega_cartridge.ram_size); puts("sega: battery restored");
        }
        free(saved);
        if (n<0 && n!=GZ_ENOENT) {
            sega_close(); free(owned);
            strlcpy(notice,"Could not restore battery save",sizeof notice); return;
        }
    }
    saved_hash=sega_hash(sega_ram(),sega_cartridge.ram_size); save_time=gz_time_ms(); save_failed=false;
    printf("sega: loaded %s ROM %u RAM %u\n",title,sega_cartridge.rom_size,sega_cartridge.ram_size);
    do {
        exit_game=reset_requested=paused=false; keys=pulse_keys=0;
        frame_count=0; start_time=gz_time_ms(); timing_reset();
        draw_game();
        while (!exit_game) {
            unsigned buttons=sega_poll();
            if (buttons&256) break;
            if (frame_count==1) sega_audio_open(22050);
            if (!sega_frame(buttons,(frame_count&1)==0)) {
                strlcpy(notice,"Cartridge stopped: CPU fault",sizeof notice); break;
            }
            if (!(frame_count&1)) sega_video(sega_pixels());
        }
        sega_audio_close();
        save_battery();
        if (reset_requested) { sega_reset(); puts("sega: reset"); }
    } while (reset_requested);
    if (save_failed && !*notice) strlcpy(notice,"Could not save battery data",sizeof notice);
    sega_close(); free(owned);
}

int main(void)
{
    ui_init(SB_STATUS_BLACK);
    puts("sega: ready"); browse(); draw_library();
    for (;;) {
        struct gz_input_event e;
        bool dirty=false;
        if (ui_next_event(&e,20)) {
            if (e.type==GZ_EV_APP_ACTIVATE) {
                dirty=true;
            } else if (e.type==GZ_EV_TOUCH_UP && *directory && gfx_rect_contains(back_button,e.x,e.y)) {
                char *slash=strrchr(directory,'/');
                if (slash==directory || !slash) directory[0]=0; else *slash=0;
                page_skip=0; browse(); dirty=true;
            } else if (e.type==GZ_EV_TOUCH_UP && more && gfx_rect_contains(gfx_r(130,278,100,24),e.x,e.y)) {
                page_skip=next_skip; browse(); dirty=true;
            } else if (e.type==GZ_EV_TOUCH_UP && page_skip && gfx_rect_contains(gfx_r(10,278,100,24),e.x,e.y)) {
                page_skip=0; browse(); dirty=true;
            } else {
                int i=ui_list_event(&list,&e,&dirty);
                if (i>=0) {
                    if (!*directory && i==0) { strlcpy(title,"Glaze demo",sizeof title); play(NULL,1); }
                    else if (!*directory) { strlcpy(directory,i==1 ? "/Documents" : "/Card",sizeof directory); page_skip=0; browse(); }
                    else {
                        char path[FS_PATH_MAX];
                        if (snprintf(path,sizeof path,"%s/%s",directory,entries[i].name)>=(int)sizeof path) strlcpy(notice,"Path is too long",sizeof notice);
                        else if (entries[i].type==FS_DIR) { strlcpy(directory,path,sizeof directory); page_skip=0; browse(); }
                        else { strlcpy(title,entries[i].name,sizeof title); play(path,false); }
                    }
                    dirty=true;
                }
            }
        }
        if (ui_scroll_step(&list.sc)) dirty=true;
        if (dirty) draw_library();
    }
}
