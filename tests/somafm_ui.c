/* Production UI, persistence and input handling against a fake audio peer. */
#define main somafm_app_main
#include "../packages/somafm/main.c"
#undef main
#include "hostres.h"
#include <assert.h>
#include <stdlib.h>

struct ui_app ui;
static uint16_t pixels[UI_W*UI_H];
static struct radio_state peer;
static char saved[4096];
static int saved_len, plays, stops, capability;
uint32_t gz_time_ms(void) { static uint32_t ms; return ms+=16; }
int gz_log(const char *s,uint32_t n) { return (int)n; }
int gz_sysinfo(struct gz_sysinfo *si) { memset(si,0,sizeof(*si)); return 0; }
void ui_init(int style) { (void)style; }
bool ui_in_background(void) { return false; }
int ui_next_event(struct gz_input_event *e,int timeout) { return 0; }
void ui_invalidate_all(void) {}
void ui_present(void) {}
int ui_ease_out(uint32_t t,uint32_t duration) { return t>=duration ? 1024 : (int)(t*1024/duration); }
const char *res_bundle(void) { return "/Applications/somafm.app"; }
int fs_read_file(const char *path,void *data,uint32_t n)
{
    if (n > (uint32_t)saved_len) n=(uint32_t)saved_len;
    memcpy(data,saved,n); return (int)n;
}
int fs_write_file(const char *path,const void *data,uint32_t n)
{
    assert(n<sizeof(saved)); memcpy(saved,data,n); saved_len=(int)n; return (int)n;
}
int fs_mkdirs(const char *path) { return 0; }
int somafm_state(struct radio_state *s) { *s=peer; return capability; }
int somafm_play(const struct radio_request *r)
{
    plays++; strcpy(peer.id,r->id); strcpy(peer.name,r->name); peer.phase=RADIO_CONNECTING; return 0;
}
int somafm_stop(void) { stops++; peer.phase=RADIO_STOPPED; return 0; }
static void tap(gfx_rect r)
{
    struct gz_input_event e={.type=GZ_EV_TOUCH_DOWN,.x=r.x+r.w/2,.y=r.y+r.h/2};
    input(&e); draw(); e.type=GZ_EV_TOUCH_UP; input(&e); draw();
}
static void snapshot(const char *name)
{
    char file[128]; snprintf(file,sizeof(file),"../out/host/somafm-%s.bmp",name);
    draw(); host_write_bmp(file,&ui.surf);
}
int main(void)
{
    gfx_surface_init(&ui.surf,pixels,UI_W,UI_H,UI_W);
    ui_list_init(&list,gfx_r(0,UI_NAV_H+38,UI_W,UI_H-UI_NAV_H-80),50);
    selected=station_index("groovesalad"); assert(selected>=0); rebuild();
    assert(nrows==STATION_COUNT && STATION_COUNT==51);
    snapshot("stations");
    view=V_NOW; draw();
    tap(favorite); assert(favorites[selected]);
    memset(favorites,0,sizeof(favorites)); selected=0; load();
    assert(!strcmp(stations[selected].id,"groovesalad") && favorites[selected]);
    tap(play); assert(plays==1 && state.phase==RADIO_CONNECTING);
    peer.phase=RADIO_PLAYING; peer.bitrate=128; strcpy(peer.title,"Fixture Artist - A song playing live");
    poll_state(); snapshot("playing");
    assert(favorite.y+favorite.h<=UI_H && play.y>214);
    struct gz_input_event e={.type=GZ_EV_TOUCH_DOWN,.x=play.x+5,.y=play.y+5};
    input(&e); e.type=GZ_EV_TOUCH_CANCEL; input(&e); e.type=GZ_EV_TOUCH_UP; input(&e);
    assert(stops==0);
    tap(play); assert(stops==1);
    peer.phase=RADIO_ERROR; peer.error=RADIO_E_NETWORK; poll_state(); snapshot("no-network");
    action(1); draw(); tap(filter_favs); assert(nrows==1 && favorites_only); snapshot("favorites");
    choose(0); assert(plays==2 && view==V_NOW);
    action(4); action(1); assert(nrows==0); snapshot("empty-favorites");
    view=V_ABOUT; snapshot("about");
    capability=GZ_EINVAL; poll_state(); assert(!compatible);
    int count=plays; toggle(); assert(count==plays);
    view=V_NOW; snapshot("old-os");
    puts("SomaFM UI: touch cancellation, favorites persistence, filtering, playback controls and old OS PASS");
    return 0;
}
