/* NES app: ROM library, portrait display, controls and eager battery saves. */
#include "core.h"
#include <glaze/audio.h>
#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LIST_MAX 64
#define VIDEO_H 225
#define VIDEO_TOP (UI_NAV_H + (UI_H - UI_NAV_H - VIDEO_H) / 2)
static struct fs_dirent entries[LIST_MAX];
static struct ui_list list;
static int entry_count, page_skip, next_skip;
static bool more, paused, reset_requested, sound_enabled = true, stream_open;
static bool exit_game, save_failed;
static char directory[FS_PATH_MAX], notice[96], title[FS_NAME_MAX], save_path[FS_PATH_MAX];
static gfx_rect back_button, pause_button;
static uint8_t keys, pulse_keys;
static uint32_t saved_hash, save_time, frame_count, start_time, frame_deadline;
static unsigned rate;

void nes_log(const char *s) { printf("nes: %s\n", s); }
void nes_audio_close(void)
{
    if (stream_open) audio_stream_close();
    stream_open = false;
}
void nes_audio_open(unsigned hz)
{
    rate = hz;
    if (sound_enabled) stream_open = audio_stream_open(hz,1) >= 0;
}
void nes_audio(const int16_t *pcm, unsigned samples)
{
    if (!stream_open || paused) return;
    /* Bound buffering to keep controls responsive if audio stalls/takes over. */
    uint32_t queued, played;
    if (audio_stream_clock(&played,&queued) >= 0 && queued > 100) return;
    int n = audio_stream_write(pcm, samples * 2);
    if (n < 0) { nes_audio_close(); return; }
    if (n > 0) audio_stream_write(NULL,0);
}

static void save_battery(void)
{
    if (!nes_cartridge.battery || !*save_path) return;
    uint32_t hash = nes_hash(nes_sram(),NES_SAVE_SIZE);
    if (hash == saved_hash) return;
    char folder[FS_PATH_MAX], temp[FS_PATH_MAX];
    snprintf(folder,sizeof folder,"%s/data",res_bundle());
    if (snprintf(temp,sizeof temp,"%s.tmp",save_path) >= (int)sizeof temp) return;
    int rv=fs_mkdirs(folder);
    if (rv>=0) rv=fs_write_file(temp,nes_sram(),NES_SAVE_SIZE);
    if (rv==NES_SAVE_SIZE) {
        rv=fs_rename(temp,save_path);
        /* FatFs does not replace destinations. Keep the completed temp until
         * the new file is renamed; the loader recovers it after interrupted replacement. */
        if (rv==GZ_EEXIST && fs_remove(save_path)>=0) rv=fs_rename(temp,save_path);
    } else rv=GZ_EIO;
    if (rv<0) {
        if (!save_failed) puts("nes: battery save failed");
        save_failed = true;
        return;
    }
    saved_hash = hash; save_failed = false;
    puts("nes: battery saved");
}

static void draw_game(void)
{
    gfx_fill(&ui.surf,gfx_r(0,UI_NAV_H,UI_W,UI_H-UI_NAV_H),0x000000);
    ui_draw_navbar(&ui.surf,paused ? "Paused" : "NES","Library",&back_button,1);
    pause_button=ui_draw_bar_button(&ui.surf,paused ? "Play" : "Pause",true,false,true,1);
    if (paused) {
        gfx_fill_alpha(&ui.surf,gfx_r(0,UI_NAV_H,UI_W,UI_H-UI_NAV_H),0x101827,230);
        gfx_text_center(&ui.surf,0,90,240,ui_font(FONT_B17),title,0xffffff,255);
        ui_draw_button(&ui.surf,gfx_r(65,132,110,32),"Reset",false);
        ui_draw_button(&ui.surf,gfx_r(65,174,110,32),sound_enabled ? "Sound On" : "Sound Off",false);
        ui_draw_button(&ui.surf,gfx_r(20,216,95,32),"Start",false);
        ui_draw_button(&ui.surf,gfx_r(125,216,95,32),"Select",false);
        if (save_failed) gfx_text_center(&ui.surf,0,264,240,ui_font(FONT_R11),"Could not save battery RAM",0xffbbbb,255);
    }
    ui_invalidate_all(); ui_present();
}

void nes_video(const uint16_t *pixels)
{
    if (paused) return;
    /* Preserve the full picture and aspect ratio, centred below navigation. */
    for (int y=0;y<VIDEO_H;y++) {
        uint16_t *out=ui.surf.px+(y+VIDEO_TOP)*ui.surf.stride;
        const uint16_t *in=pixels+(y*16/15)*256;
        for (int x=0;x<240;x++) out[x]=nes_rgb565(in[x*16/15]);
    }
    ui_invalidate(gfx_r(0,VIDEO_TOP,UI_W,VIDEO_H)); ui_present();
}

static uint8_t key_bit(unsigned code)
{
    switch(code) {
    case GZ_KEY_UP:return NES_UP; case GZ_KEY_DOWN:return NES_DOWN;
    case GZ_KEY_LEFT:return NES_LEFT; case GZ_KEY_RIGHT:return NES_RIGHT;
    case GZ_KEY_OK:return NES_A; case GZ_KEY_ESC:return NES_B;
    case GZ_KEY_ENTER:return NES_START; case GZ_KEY_TAB:return NES_SELECT;
    default:return 0;
    }
}

static void resume_game(void)
{
    paused=false;
    nes_audio_open(rate);
    frame_deadline=gz_time_ms();
    puts("nes: resumed");
    draw_game();
}

static void game_event(const struct gz_input_event *e)
{
    if (e->type==GZ_EV_APP_DEACTIVATE) {
        keys=pulse_keys=0;
        save_battery();
        nes_audio_close();
        puts("nes: backgrounded");
    } else if (e->type==GZ_EV_APP_ACTIVATE) {
        frame_deadline=gz_time_ms();
        if (!paused) nes_audio_open(rate);
        draw_game();
        puts("nes: foregrounded");
    } else if (e->type==GZ_EV_TOUCH_CANCEL) {
        keys=pulse_keys=0;
    } else if (e->type==GZ_EV_KEY_DOWN || e->type==GZ_EV_KEY_UP) {
        uint8_t b=key_bit(e->code);
        if (e->type==GZ_EV_KEY_DOWN) { keys|=b; pulse_keys|=b; } else keys&=~b;
    } else if (e->type==GZ_EV_TOUCH_UP) {
        if (gfx_rect_contains(back_button,e->x,e->y)) { exit_game=true; save_battery(); }
        else if (gfx_rect_contains(pause_button,e->x,e->y)) {
            keys=pulse_keys=0;
            if (!paused) {
                paused=true; save_battery(); nes_audio_close(); puts("nes: paused");
                draw_game();
            } else resume_game();
        } else if (paused && gfx_rect_contains(gfx_r(65,132,110,32),e->x,e->y)) {
            save_battery(); reset_requested=true; exit_game=true;
        } else if (paused && gfx_rect_contains(gfx_r(65,174,110,32),e->x,e->y)) {
            sound_enabled=!sound_enabled; draw_game();
        } else if (paused && gfx_rect_contains(gfx_r(20,216,95,32),e->x,e->y)) {
            pulse_keys|=NES_START; puts("nes: Start"); resume_game();
        } else if (paused && gfx_rect_contains(gfx_r(125,216,95,32),e->x,e->y)) {
            pulse_keys|=NES_SELECT; puts("nes: Select"); resume_game();
        }
    }
}

unsigned nes_poll(void)
{
    struct gz_input_event e;
    while (ui_next_event(&e,0)) game_event(&e);
    while (paused && !exit_game) {
        if (ui_next_event(&e,100)) game_event(&e);
    }
    if (exit_game) return 256;
    uint32_t now=gz_time_ms();
    if ((uint32_t)(now-save_time)>=5000) { save_battery(); save_time=now; }
    /* Rational 60 Hz deadline; no accumulated catch-up after a long pause/stall. */
    frame_count++;
    frame_deadline += frame_count % 3 ? 17 : 16;
    if ((int32_t)(frame_deadline-now)>0) {
        int wait=(int32_t)(frame_deadline-now);
        if (wait>20) wait=20;
        while (wait>0 && ui_next_event(&e,wait)) {
            game_event(&e);
            wait=(int32_t)(frame_deadline-gz_time_ms());
        }
    } else if ((int32_t)(now-frame_deadline)>100) frame_deadline=now;
    if (frame_count % 300 == 0) {
        uint32_t elapsed=gz_time_ms()-start_time;
        printf("nes: %u frames in %u ms (%u fps)\n",frame_count,elapsed,elapsed ? frame_count*1000/elapsed : 0);
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
                if (batch[i].type==FS_DIR || (dot && !strcasecmp(dot,".nes"))) entries[entry_count++]=batch[i];
            }
            skip+=n; scanned+=n;
            if (n<8) break;
            if (scanned==LIST_MAX) more=true;
        }
        next_skip=skip;
        qsort(entries,entry_count,sizeof entries[0],entry_compare);
    }
    if (!entry_count && !more && !*notice) strlcpy(notice,"No .nes ROMs in this folder",sizeof notice);
    ui_list_init(&list,gfx_r(0,30,240,230),34);
    ui_list_reset(&list,entry_count);
}

static void draw_row(gfx_surface *s,int i,gfx_rect r,bool selected,void *ctx)
{
    (void)ctx; ui_draw_list_row(s,r,selected);
    gfx_text(s,r.x+10,r.y+9,ui_font(FONT_R13),entries[i].name,UI_TEXT,255);
    if (entries[i].type==FS_DIR || (!*directory && i)) ui_draw_chevron(s,r.x+r.w-15,r.y+17,UI_TEXT_DETAIL);
}

static void draw_library(void)
{
    gfx_fill_alpha(&ui.surf,gfx_r(0,0,240,304),0xe8ecf0,255);
    ui_draw_navbar(&ui.surf,*directory ? strrchr(directory,'/')+1 : "NES",*directory ? "Back" : NULL,&back_button,0);
    ui_list_draw(&ui.surf,&list,draw_row,NULL);
    const gfx_font *f=ui_font(FONT_R10);
    gfx_text_center(&ui.surf,0,263,240,f,*notice ? notice : "Open an NTSC .nes ROM. Arrows + OK / Esc.",UI_TEXT_DETAIL,255);
    if (more) ui_draw_button(&ui.surf,gfx_r(130,278,100,24),"More...",false);
    if (page_skip) ui_draw_button(&ui.surf,gfx_r(10,278,100,24),"First page",false);
    if (!*directory) gfx_text_center(&ui.surf,0,284,240,f,"Enter: Start    Tab: Select",UI_TEXT_DETAIL,255);
    ui_invalidate_all(); ui_present();
}

static void play(const char *path,bool demo)
{
    uint8_t *owned=NULL;
    const uint8_t *data;
    uint32_t size;
    if (demo) data=res_map("demo.nes",&size);
    else {
        struct fs_stat st;
        if (fs_stat(path,&st)<0 || st.type!=FS_FILE || st.size>NES_ROM_MAX) {
            strlcpy(notice,"Cannot open ROM (maximum 1 MiB)",sizeof notice); return;
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
    const char *error=nes_open(data,size);
    free(owned);
    if (error) { strlcpy(notice,error,sizeof notice); return; }
    if (snprintf(save_path,sizeof save_path,"%s/data/%08x.sav",res_bundle(),nes_cartridge.hash)>=(int)sizeof save_path) {
        nes_close(); strlcpy(notice,"App bundle path is too long",sizeof notice); return;
    }
    if (nes_cartridge.battery) {
        uint8_t *save=malloc(NES_SAVE_SIZE);
        if (save) {
            int n=fs_read_file(save_path,save,NES_SAVE_SIZE);
            if (n!=NES_SAVE_SIZE) {
                char temp[FS_PATH_MAX];
                if (snprintf(temp,sizeof temp,"%s.tmp",save_path)<(int)sizeof temp) n=fs_read_file(temp,save,NES_SAVE_SIZE);
            }
            if (n==NES_SAVE_SIZE) { memcpy(nes_sram(),save,NES_SAVE_SIZE); puts("nes: battery restored"); }
            free(save);
        }
    }
    saved_hash=nes_hash(nes_sram(),NES_SAVE_SIZE); save_time=gz_time_ms(); save_failed=false;
    printf("nes: loaded %s mapper %u, PRG %u CHR %u\n",title,nes_cartridge.mapper,nes_cartridge.prg_size,nes_cartridge.chr_size);
    do {
        exit_game=reset_requested=paused=false; keys=pulse_keys=0;
        frame_count=0; frame_deadline=start_time=gz_time_ms();
        draw_game(); nes_run();
        save_battery();
        if (reset_requested) { nes_reset(); puts("nes: reset"); }
    } while (reset_requested);
    nes_close();
}

int main(void)
{
    ui_init(SB_STATUS_BLACK);
    puts("nes: ready"); browse(); draw_library();
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
                    if (!*directory && i==0) { strlcpy(title,"Glaze demo",sizeof title); play(NULL,true); }
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
