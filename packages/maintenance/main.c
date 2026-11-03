/* Offline installer companion. Uses the existing USB file/run protocol. */
#include <glaze/fs.h>
#include <glaze/pkg.h>
#include <glaze/sys.h>
#include <glaze/ota.h>
#include <glaze/ui.h>
#include <glaze/builtin_apps.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#define BASE "/Library/Installer/"
static char response[8192];
static unsigned used;
static const char *const builtin_paths[] = {
#define BUILTIN_PATH(label, icon, path, style, dock) path,
    GZ_BUILTIN_APPS(BUILTIN_PATH)
#undef BUILTIN_PATH
    "bin/wince"
};
static bool present(const char *path)
{
    /* ROMFS cannot change while this process runs. Avoid accumulating mappings
     * when a batch installation emits many inventory replies. */
    static const char *names[16];
    static bool exists[16];
    static unsigned count;
    for(unsigned i=0;i<count;i++)if(!strcmp(names[i],path))return exists[i];
    uint32_t size=0;
    bool found=gz_romfs_map(path,&size)!=NULL;
    if(count<16) { names[count]=path; exists[count++]=found; }
    return found;
}
static int uninstall(const char *id)
{
    /* Only validated package IDs, never paths or built-in/system programs. */
    size_t len=strlen(id);
    if(!len || len>PKG_ID_MAX || !strcmp(id,"maintenance"))return GZ_EINVAL;
    for(size_t i=0;i<len;i++)
        if(!strchr("abcdefghijklmnopqrstuvwxyz0123456789.-",id[i]))return GZ_EINVAL;
    for(unsigned i=0;i<sizeof(builtin_paths)/sizeof(builtin_paths[0]);i++)
        if(!strcmp(id,builtin_paths[i]+4))return GZ_EPERM;
    static const char *const services[]={"fs","net","springboard","usb","audio","ld-glaze"};
    for(unsigned i=0;i<sizeof(services)/sizeof(services[0]);i++)
        if(!strcmp(id,services[i]))return GZ_EPERM;
    char bundle[PKG_BUNDLE_MAX]; struct pkg_info info;
    int rv=pkg_find(id,bundle);
    if(rv==GZ_ENOENT)return 1;
    if(rv)return rv;
    if((rv=pkg_bundle_info(bundle,&info)))return rv;
    if(strcmp(info.id,id))return GZ_EINVAL;
    return pkg_uninstall(bundle);
}
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
    if (used < sizeof(response)-1024)
        used += snprintf(response+used,sizeof(response)-used,"%s\"%s\":\"%s\"", response[used-1]=='{'?"":",",info->id,info->version);
}
static void reply(const char *id, int code, const char *phase)
{
    struct gz_ota_status ota={0};
    struct fs_info fs={0};
    gz_ota_status(&ota); fs_statfs(&fs);
    used=snprintf(response,sizeof(response),"{\"id\":\"%s\",\"code\":%d,\"phase\":\"%s\",\"flags\":%u,\"version\":\"%s\",\"build\":\"%s\",\"last_result\":%u,\"free\":%u,\"apps\":{",id,code,phase,ota.flags,ota.version,ota.build,ota.last_result,fs.total>=fs.used?fs.total-fs.used:0);
    pkg_list("",app_info,NULL); pkg_list("/Card",app_info,NULL);
    used += snprintf(response+used,sizeof(response)-used,"},\"protocol\":2,\"builtins\":[");
    bool first=true;
    for(unsigned i=0;i<sizeof(builtin_paths)/sizeof(builtin_paths[0]);i++)
        if(present(builtin_paths[i])) {
            used+=snprintf(response+used,sizeof(response)-used,"%s\"%s\"",first?"":",",builtin_paths[i]+4);
            first=false;
        }
    struct gz_power_status power={0}; gz_power_status(&power);
    used += snprintf(response+used,sizeof(response)-used,"],\"wl54cf\":%s,\"power_flags\":%u,\"battery\":%u}\n",
        present("firmware/cf8385_helper.bin") && present("firmware/cf8385.bin")?"true":"false",power.flags,power.percent);
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
        char req[96]={0}, id[33]={0}, action[64]={0};
        int n=fs_read_file(BASE "request",req,sizeof(req)-1);
        if(n>0) {
            fs_remove(BASE "request");
            char *space=strchr(req,' ');
            if(!space || space-req!=32 || n>= (int)sizeof(req)-1 || (size_t)(n-33)>=sizeof(action))continue;
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
            } else if(!strncmp(action,"uninstall:",10)) {
                show_status("Removing app...");
                rv=uninstall(action+10);
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
