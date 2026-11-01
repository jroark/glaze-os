/* Offline installer companion. Uses the existing USB file/run protocol. */
#include <glaze/fs.h>
#include <glaze/pkg.h>
#include <glaze/sys.h>
#include <glaze/ota.h>
#include <glaze/ui.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#define BASE "/Library/Installer/"
static char response[8192];
static unsigned used;
static void show_status(const char *message)
{
    gfx_fill(&ui.surf,gfx_r(0,0,UI_W,UI_H),0xeeeeee);
    ui_draw_navbar(&ui.surf,"Glaze Installer",NULL,NULL,0);
    gfx_text_center(&ui.surf,0,100,UI_W,ui_font(FONT_B14),message,UI_TEXT,255);
    gfx_text_center(&ui.surf,0,140,UI_W,ui_font(FONT_R12),"Follow your computer.",UI_TEXT,255);
    gfx_text_center(&ui.surf,0,160,UI_W,ui_font(FONT_R12),"Keep power connected.",UI_TEXT,255);
    ui_invalidate_all(); ui_present();
}
static void app_info(const char *bundle, const struct pkg_info *info, void *arg)
{
    (void)bundle; (void)arg;
    /* IDs and versions are validated by the package library. */
    if (used < sizeof(response)-128)
        used += snprintf(response+used,sizeof(response)-used,"%s\"%s\":\"%s\"", response[used-1]=='{'?"":",",info->id,info->version);
}
static void reply(const char *id, int code, const char *phase)
{
    struct gz_ota_status ota={0};
    struct fs_info fs={0};
    gz_ota_status(&ota); fs_statfs(&fs);
    used=snprintf(response,sizeof(response),"{\"id\":\"%s\",\"code\":%d,\"phase\":\"%s\",\"flags\":%u,\"version\":\"%s\",\"build\":\"%s\",\"last_result\":%u,\"free\":%u,\"apps\":{",id,code,phase,ota.flags,ota.version,ota.build,ota.last_result,fs.total>=fs.used?fs.total-fs.used:0);
    pkg_list("",app_info,NULL); pkg_list("/Card",app_info,NULL);
    used += snprintf(response+used,sizeof(response)-used,"}}\n");
    fs_write_file(BASE "result.tmp",response,used);
    fs_remove(BASE "result"); fs_rename(BASE "result.tmp",BASE "result");
}
int main(void)
{
    ui_init(SB_STATUS_GRAY);
    show_status("Connected");
    /* Resolve OTA entrypoints at runtime so app installation works on older OSes. */
    int (*begin)(const char *)=dlsym(NULL,"fs_ota_begin");
    int (*step)(void)=dlsym(NULL,"fs_ota_step");
    int (*arm)(void)=dlsym(NULL,"fs_ota_arm");
    int (*cancel)(void)=dlsym(NULL,"fs_ota_cancel");
    uint32_t last=gz_time_ms();
    for (;;) {
        struct gz_input_event e; while(ui_next_event(&e,0)) {}
        char req[96]={0}, id[33]={0}, action[16]={0};
        int n=fs_read_file(BASE "request",req,sizeof(req)-1);
        if(n>0) {
            fs_remove(BASE "request");
            char *space=strchr(req,' ');
            if(!space || space-req!=32)continue;
            bool valid=true; for(int i=0;i<32;i++)if(!strchr("0123456789abcdef",req[i]))valid=false;
            if(!valid)continue;
            memcpy(id,req,32); strlcpy(action,space+1,sizeof(action)); last=gz_time_ms();
            int rv=0;
            if(!strcmp(action,"quit")) {
                fs_remove(BASE "app.gzp"); fs_remove(BASE "maintenance.gzp");
                fs_remove(BASE "app.gzp.part"); fs_remove(BASE "request.part");
                fs_remove("/Library/Updates/installer.gzu.part");
                fs_remove("/Library/Updates/installer.gzu");
                pkg_uninstall("/Applications/maintenance.app");
                reply(id,0,"done");
                return 0;
            }
            if(!strcmp(action,"install")) {
                show_status("Installing app...");
                struct pkg_info incoming,existing; char bundle[PKG_BUNDLE_MAX], root[8]="";
                rv=pkg_read_info(BASE "app.gzp",&incoming);
                if(!rv && !strcmp(incoming.id,"maintenance"))rv=GZ_EINVAL;
                if(!rv && !pkg_find(incoming.id,bundle)) {
                    rv=pkg_bundle_info(bundle,&existing);
                    if(!rv && gz_version_cmp(incoming.version,existing.version)<=0)rv=1;
                    if(!strncmp(bundle,"/Card/",6))strlcpy(root,"/Card",sizeof(root));
                }
                if(!rv)rv=pkg_install(BASE "app.gzp",root,bundle);
                fs_remove(BASE "app.gzp");
            } else if(!strcmp(action,"update")) {
                show_status("Verifying OS update...");
                struct gz_power_status power={0};
                struct gz_ota_status ota={0};
                if(!begin || !step || !arm || !cancel || gz_ota_status(&ota) || !(ota.flags&GZ_OTA_S_SUPPORTED))rv=GZ_EINVAL;
                else if(gz_power_status(&power) || (!(power.flags&GZ_POWER_AC) && (!(power.flags&GZ_POWER_VALID) || power.percent<50)))rv=GZ_EPERM;
                else {
                    int size=begin("/Library/Updates/installer.gzu"); rv=size;
                    if(size>=0) { rv=0; while(rv>=0 && rv<size) { rv=step(); while(ui_next_event(&e,0)){} } }
                    if(rv>=0) { reply(id,0,"arming"); rv=arm(); }
                    cancel();
                }
                fs_remove("/Library/Updates/installer.gzu");
            } else if(strcmp(action,"info"))rv=GZ_EINVAL;
            reply(id,rv,"done");
        }
        if((uint32_t)(gz_time_ms()-last)>600000)break;
        gz_sleep_ms(100);
    }
    fs_remove(BASE "request"); fs_remove(BASE "result"); fs_remove(BASE "app.gzp");
    fs_remove(BASE "maintenance.gzp");
    pkg_uninstall("/Applications/maintenance.app");
    return 0;
}
