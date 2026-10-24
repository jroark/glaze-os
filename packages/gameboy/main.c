/* Game Boy and Color app: ROM library, portrait display, controls and eager battery saves. */
#include "gameboy.h"
#include "save.h"
#include <glaze/notify.h>
#include <glaze/audio.h>
#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LIST_MAX 64
#define VIDEO_H 216
#define VIDEO_TOP (UI_NAV_H + (UI_H - UI_NAV_H - VIDEO_H) / 2)
static struct fs_dirent entries[LIST_MAX];
static struct ui_list list;
static int entry_count, page_skip, next_skip;
static bool more, paused, reset_requested, sound_enabled = true, stream_open;
static bool exit_game, save_failed;
static char directory[FS_PATH_MAX], notice[96], title[FS_NAME_MAX], save_path[FS_PATH_MAX];
static gfx_rect back_button, pause_button;
static uint8_t keys, pulse_keys;
static uint32_t saved_hash, saved_rtc_hash, save_time, frame_count, start_time;
static uint64_t frame_deadline;
static uint32_t frame_fraction;
static unsigned rate=22050;
static uint32_t wall_time(void) {
    struct gz_datetime dt;
    return gz_rtc_get(&dt)>=0 ? gz_dt_to_secs(&dt) : 0;
}
void gb_serial(uint8_t value) { (void)value; }
static void timing_reset(void) { frame_deadline=gz_time_us(); frame_fraction=0; }


void gb_log(const char *s) { printf("gameboy: %s\n", s); }
void gb_audio_close(void)
{
    if (stream_open) audio_stream_close();
    stream_open = false;
}
void gb_audio_open(unsigned hz)
{
    rate = hz;
    if (stream_open) return;
    if (sound_enabled) stream_open = audio_stream_open(hz,1) >= 0;
}
void gb_audio(const int16_t *pcm, unsigned samples)
{
    if (!stream_open || paused) return;
    /* Bound buffering to keep controls responsive if audio stalls/takes over. */
    uint32_t queued, played;
    if (audio_stream_clock(&played,&queued) >= 0 && queued > 100) return;
    int n = audio_stream_write(pcm, samples * 2);
    if (n < 0) { gb_audio_close(); return; }
    if (n > 0) audio_stream_write(NULL,0);
}

static void save_battery(void)
{
    if (!gb_cartridge.battery || !*save_path) return;
    char folder[FS_PATH_MAX], clock_path[FS_PATH_MAX];
    snprintf(folder,sizeof folder,"%s/data",res_bundle());
    if (fs_mkdirs(folder)<0) {
        if (!save_failed) puts("gameboy: battery save failed");
        save_failed=true; return;
    }
    uint32_t hash=gb_hash(gb_ram(),gb_cartridge.ram_size);
    int rv=0;
    if (gb_cartridge.ram_size && hash!=saved_hash) {
        rv=gb_save_write(save_path,gb_ram(),gb_cartridge.ram_size);
        if (rv>=0) { saved_hash=hash; puts("gameboy: battery saved"); }
    }
    if (gb_cartridge.clock) {
        uint8_t data[GB_RTC_SIZE];
        gb_clock_export(data,wall_time());
        uint32_t h=gb_hash(data,sizeof data);
        if (h!=saved_rtc_hash) {
            if (snprintf(clock_path,sizeof clock_path,"%s.rtc",save_path)>=(int)sizeof clock_path) rv=GZ_EINVAL;
            else {
                int result=gb_save_write(clock_path,data,sizeof data);
                if (result<0) rv=result; else saved_rtc_hash=h;
            }
        }
    }
    if (rv<0 && !save_failed) puts("gameboy: battery save failed");
    save_failed=rv<0;
}

static void draw_game(void)
{
    gfx_fill(&ui.surf,gfx_r(0,UI_NAV_H,UI_W,UI_H-UI_NAV_H),0x000000);
    ui_draw_navbar(&ui.surf,paused ? "Paused" : "Game Boy","Library",&back_button,1);
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
    if (!paused && gb_cartridge.checksum_warning) gfx_text_center(&ui.surf,0,286,240,ui_font(FONT_R10),"Header checksum warning",0xffcc88,255);
    ui_invalidate_all(); ui_present();
}

void gb_video(const uint16_t *pixels)
{
    if (paused) return;
    /* Preserve the full picture and aspect ratio, centred below navigation. */
    for (int y=0;y<VIDEO_H;y++) {
        uint16_t *out=ui.surf.px+(y+VIDEO_TOP)*ui.surf.stride;
        const uint16_t *in=pixels+(y*2/3)*160;
        for (int x=0;x<240;x++) out[x]=in[x*2/3];
    }
    ui_invalidate(gfx_r(0,VIDEO_TOP,UI_W,VIDEO_H));
    if (save_failed) {
        gfx_text_center(&ui.surf,0,286,240,ui_font(FONT_R10),"Could not save battery data",0xffbbbb,255);
        ui_invalidate(gfx_r(0,280,UI_W,24));
    }
    ui_present();
}

static uint8_t key_bit(unsigned code)
{
    switch(code) {
    case GZ_KEY_UP:return GB_UP; case GZ_KEY_DOWN:return GB_DOWN;
    case GZ_KEY_LEFT:return GB_LEFT; case GZ_KEY_RIGHT:return GB_RIGHT;
    case GZ_KEY_OK:return GB_A; case GZ_KEY_ESC:return GB_B;
    case GZ_KEY_ENTER:return GB_START; case GZ_KEY_TAB:return GB_SELECT;
    default:return 0;
    }
}

static void resume_game(void)
{
    paused=false;
    gb_audio_open(rate);
    timing_reset();
    puts("gameboy: resumed");
    draw_game();
}

static void game_event(const struct gz_input_event *e)
{
    if (e->type==GZ_EV_APP_DEACTIVATE) {
        keys=pulse_keys=0;
        save_battery();
        gb_audio_close();
        puts("gameboy: backgrounded");
    } else if (e->type==GZ_EV_APP_ACTIVATE) {
        timing_reset();
        if (!paused) gb_audio_open(rate);
        draw_game();
        puts("gameboy: foregrounded");
    } else if (e->type==GZ_EV_TOUCH_CANCEL) {
        keys=pulse_keys=0;
    } else if (e->type==GZ_EV_KEY_DOWN || e->type==GZ_EV_KEY_UP) {
        uint8_t b=key_bit(e->code);
        if (e->type==GZ_EV_KEY_DOWN) { if (b) printf("gameboy: input %u\n",(unsigned)b); keys|=b; pulse_keys|=b; } else keys&=~b;
    } else if (e->type==GZ_EV_TOUCH_UP) {
        if (gfx_rect_contains(back_button,e->x,e->y)) { exit_game=true; save_battery(); }
        else if (gfx_rect_contains(pause_button,e->x,e->y)) {
            keys=pulse_keys=0;
            if (!paused) {
                paused=true; save_battery(); gb_audio_close(); puts("gameboy: paused");
                draw_game();
            } else resume_game();
        } else if (paused && gfx_rect_contains(gfx_r(65,132,110,32),e->x,e->y)) {
            save_battery(); reset_requested=true; exit_game=true;
        } else if (paused && gfx_rect_contains(gfx_r(65,174,110,32),e->x,e->y)) {
            sound_enabled=!sound_enabled; puts(sound_enabled ? "gameboy: sound on" : "gameboy: sound off"); draw_game();
        } else if (paused && gfx_rect_contains(gfx_r(20,216,95,32),e->x,e->y)) {
            pulse_keys|=GB_START; puts("gameboy: Start"); resume_game();
        } else if (paused && gfx_rect_contains(gfx_r(125,216,95,32),e->x,e->y)) {
            pulse_keys|=GB_SELECT; puts("gameboy: Select"); resume_game();
        }
    }
}

static unsigned gb_poll(void)
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
    /* 70,224 clocks per frame at 4,194,304 Hz; preserve fractional time. */
    frame_count++;
    frame_fraction += 1481216u;
    frame_deadline += 16742 + (frame_fraction/2097152u);
    frame_fraction %= 2097152u;
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
        printf("gameboy: %u frames in %u ms (%u fps)\n",frame_count,elapsed,elapsed ? frame_count*1000/elapsed : 0);
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
        const char *names[]={"Glaze DMG demo","Glaze Color demo","Documents","Card"};
        for (int i=0;i<4;i++) { memset(&entries[i],0,sizeof entries[i]); strlcpy(entries[i].name,names[i],FS_NAME_MAX); }
        entry_count=4;
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
                if (batch[i].type==FS_DIR || (dot && (!strcasecmp(dot,".gb") || !strcasecmp(dot,".gbc")))) entries[entry_count++]=batch[i];
            }
            skip+=n; scanned+=n;
            if (n<8) break;
            if (scanned==LIST_MAX) more=true;
        }
        next_skip=skip;
        qsort(entries,entry_count,sizeof entries[0],entry_compare);
    }
    if (!entry_count && !more && !*notice) strlcpy(notice,"No .gb or .gbc ROMs in this folder",sizeof notice);
    ui_list_init(&list,gfx_r(0,30,240,230),34);
    ui_list_reset(&list,entry_count);
}

static void draw_row(gfx_surface *s,int i,gfx_rect r,bool selected,void *ctx)
{
    (void)ctx; ui_draw_list_row(s,r,selected);
    gfx_text(s,r.x+10,r.y+9,ui_font(FONT_R13),entries[i].name,UI_TEXT,255);
    if (entries[i].type==FS_DIR || (!*directory && i>=2)) ui_draw_chevron(s,r.x+r.w-15,r.y+17,UI_TEXT_DETAIL);
}

static void draw_library(void)
{
    gfx_fill_alpha(&ui.surf,gfx_r(0,0,240,304),0xe8ecf0,255);
    ui_draw_navbar(&ui.surf,*directory ? strrchr(directory,'/')+1 : "Game Boy",*directory ? "Back" : NULL,&back_button,0);
    ui_list_draw(&ui.surf,&list,draw_row,NULL);
    const gfx_font *f=ui_font(FONT_R10);
    gfx_text_center(&ui.surf,0,263,240,f,*notice ? notice : "Open .gb / .gbc. Arrows + OK / Esc.",UI_TEXT_DETAIL,255);
    if (more) ui_draw_button(&ui.surf,gfx_r(130,278,100,24),"More...",false);
    if (page_skip) ui_draw_button(&ui.surf,gfx_r(10,278,100,24),"First page",false);
    if (!*directory) gfx_text_center(&ui.surf,0,284,240,f,"Enter: Start    Tab: Select",UI_TEXT_DETAIL,255);
    ui_invalidate_all(); ui_present();
}

static void play(const char *path,int demo)
{
    uint8_t *owned=NULL;
    const uint8_t *data;
    uint32_t size;
    if (demo) data=res_map(demo==1 ? "demo.gb" : "demo.gbc",&size);
    else {
        struct fs_stat st;
        if (fs_stat(path,&st)<0 || st.type!=FS_FILE || st.size>GB_ROM_MAX) {
            strlcpy(notice,"Cannot open ROM (maximum 8 MiB)",sizeof notice); return;
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
    const char *error=gb_open(data,size,wall_time());
    if (error) { free(owned); strlcpy(notice,error,sizeof notice); return; }
    if (snprintf(save_path,sizeof save_path,"%s/data/%08x.sav",res_bundle(),gb_cartridge.hash)>=(int)sizeof save_path) {
        gb_close(); free(owned); strlcpy(notice,"App bundle path is too long",sizeof notice); return;
    }
    if (gb_cartridge.battery && gb_cartridge.ram_size) {
        uint8_t *saved=malloc(gb_cartridge.ram_size);
        int n=saved ? gb_save_read(save_path,saved,gb_cartridge.ram_size) : GZ_ENOMEM;
        if (n==(int)gb_cartridge.ram_size) {
            memcpy(gb_ram(),saved,gb_cartridge.ram_size); puts("gameboy: battery restored");
        }
        free(saved);
        if (n<0 && n!=GZ_ENOENT) {
            gb_close(); free(owned);
            strlcpy(notice,"Could not restore battery save",sizeof notice); return;
        }
    }
    saved_rtc_hash=0;
    if (gb_cartridge.clock) {
        uint8_t clock[GB_RTC_SIZE]; char path[FS_PATH_MAX];
        if (snprintf(path,sizeof path,"%s.rtc",save_path)<(int)sizeof path &&
            gb_save_read(path,clock,sizeof clock)==sizeof clock && gb_clock_import(clock,wall_time()))
            puts("gameboy: clock restored");
    }
    saved_hash=gb_hash(gb_ram(),gb_cartridge.ram_size); save_time=gz_time_ms(); save_failed=false;
    printf("gameboy: loaded %s %s MBC%u, ROM %u RAM %u\n",title,gb_cartridge.cgb ? "CGB" : "DMG",gb_cartridge.mapper,gb_cartridge.rom_size,gb_cartridge.ram_size);
    if (gb_cartridge.checksum_warning) puts("gameboy: header checksum warning");
    do {
        exit_game=reset_requested=paused=false; keys=pulse_keys=0;
        frame_count=0; start_time=gz_time_ms(); timing_reset();
        draw_game();
        while (!exit_game) {
            unsigned buttons=gb_poll();
            if (buttons&256) break;
            if (frame_count==1) gb_audio_open(22050);
            if (!gb_frame(buttons,(frame_count&1)==0,wall_time())) {
                strlcpy(notice,"Cartridge stopped: CPU fault",sizeof notice); break;
            }
            if (!(frame_count&1)) gb_video(gb_pixels());
        }
        gb_audio_close();
        save_battery();
        if (reset_requested) { gb_reset(); puts("gameboy: reset"); }
    } while (reset_requested);
    if (save_failed && !*notice) strlcpy(notice,"Could not save battery data",sizeof notice);
    gb_close(); free(owned);
}

int main(void)
{
    ui_init(SB_STATUS_BLACK);
    puts("gameboy: ready"); browse(); draw_library();
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
                    if (!*directory && i<2) { strlcpy(title,i==0 ? "Glaze DMG demo" : "Glaze Color demo",sizeof title); play(NULL,i+1); }
                    else if (!*directory) { strlcpy(directory,i==2 ? "/Documents" : "/Card",sizeof directory); page_skip=0; browse(); }
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
