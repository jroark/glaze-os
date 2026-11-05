/* Exercise real transport polling and icon layout without hardware. */
#include "settings_sys.h"
#include "../../servers/springboard/connectivity.c"
#include "hostres.h"
#include <glaze/usb_wire.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool storage;
static unsigned net_kind, net_state, card_kind, serial_state,audio_state;
static int serial_error;
bool sb_has_program(const char *path) { return true; }
int fs_stat(const char *path,struct fs_stat *st) { assert(!strcmp(path,"/Card"));return storage?0:GZ_ENOENT; }
int net_status(struct net_status *st) { memset(st,0,sizeof(*st));st->kind=net_kind;st->state=net_state;return 0; }
int gz_bt_card_info(struct bt_card_info *out,uint32_t n) { assert(n==sizeof(*out));memset(out,0,n);out->kind=card_kind;return 0; }
int bt_audio_status(struct bt_audio_status *st){memset(st,0,sizeof(*st));st->state=audio_state;return 0;}
int bt_serial_status(struct bt_serial_status *st) { memset(st,0,sizeof(*st));st->state=serial_state;return serial_error; }
int main(void)
{
    struct sb_view v={0};
    assert(!sb_update_connectivity(&v));
    storage=true;assert(sb_update_connectivity(&v) && v.card_mounted);
    assert(!gfx_rect_empty(sb_status_icon_rect(&v,SB_ICON_STORAGE)));
    storage=false;net_kind=GZ_NET_XI825;net_state=NET_CONFIGURING;
    assert(sb_update_connectivity(&v) && !v.card_mounted && !v.network_connected);
    assert(gfx_rect_empty(sb_status_icon_rect(&v,SB_ICON_STORAGE)));
    net_state=NET_UP;assert(sb_update_connectivity(&v) && v.network_connected);
    net_kind=GZ_NET_NE2000;assert(sb_update_connectivity(&v));
    net_kind=GZ_NET_USB;storage=true;
    assert(sb_update_connectivity(&v) && v.card_mounted && v.network_kind==GZ_NET_USB);
    storage=false;card_kind=BT_CARD_BLUETOOTH;
    assert(sb_update_connectivity(&v) && !v.card_mounted && v.bluetooth_present && !v.bluetooth_connected);
    serial_state=BT_SERIAL_CONNECTED;
    assert(sb_update_connectivity(&v) && v.bluetooth_connected);
    serial_error=GZ_ENOENT;
    assert(sb_update_connectivity(&v) && v.bluetooth_present && !v.bluetooth_connected);
    audio_state=BT_AUDIO_STREAMING;assert(sb_update_connectivity(&v) && v.bluetooth_connected);
    audio_state=BT_AUDIO_CLOSING;assert(sb_update_connectivity(&v) && !v.bluetooth_connected);
    serial_error=0;card_kind=BT_CARD_ABSENT;net_kind=GZ_NET_NONE;net_state=NET_NO_CARD;
    assert(sb_update_connectivity(&v) && !v.bluetooth_present && !v.network_kind);

    /* All icons remain separate with percentage and debugger visible. Render
     * the actual glyphs in every status-bar style for visual inspection. */
    sb_draw_init();
    strcpy(v.clk.status,"12:34 PM");v.power.flags=GZ_POWER_VALID;v.power.level=100;
    v.battery_pct=true;v.debugger=GU_DEBUG_CONNECTED;v.network_connected=true;
    unsigned kinds[]={GZ_NET_NONE,GZ_NET_XI825,GZ_NET_NE2000,GZ_NET_NONE,GZ_NET_USB};
    for(unsigned row=0;row<5;row++) {
        v.card_mounted=row==0 || row==4;v.network_kind=kinds[row];
        v.bluetooth_present=row>=3;v.bluetooth_connected=row>=3;
        for(unsigned a=SB_ICON_STORAGE;a<SB_ICON_COUNT;a++) {
            gfx_rect first=sb_status_icon_rect(&v,a);
            if(gfx_rect_empty(first))continue;
            assert(first.x>=83 && first.x+first.w<=214);
            for(unsigned b=a+1;b<SB_ICON_COUNT;b++) {
                gfx_rect second=sb_status_icon_rect(&v,b),overlap;
                assert(!gfx_rect_intersect(first,second,&overlap));
            }
        }
    }
    /* One sheet: storage, Wi-Fi, Ethernet, Bluetooth, USB+storage+Bluetooth. */
    static uint16_t sheet_pixels[240*240];gfx_surface sheet;
    gfx_surface_init(&sheet,sheet_pixels,240,240,240);
    for(unsigned row=0;row<5;row++) {
        v.card_mounted=row==0 || row==4;v.network_kind=kinds[row];
        v.bluetooth_present=row>=3;v.bluetooth_connected=row>=3;
        for(unsigned style=0;style<3;style++) {
            gfx_surface bar;gfx_surface_init(&bar,sheet_pixels+(row*3+style)*240*16,240,16,240);
            sb_draw_status(&bar,&v,style,false);
        }
    }
    host_write_bmp("../out/host/status-transports.bmp",&sheet);
    puts("Connectivity: storage-only CF, Wi-Fi/wired/USB/BT presence, link changes and separate icon targets PASS");
    return 0;
}
