/*
 * Installed apps (glaze/pkg.h): found under /Applications on the flash and
 * /Card/Applications on a CompactFlash card, appended to sb_apps[] after the
 * built-in ones. A slot keeps its index for the session, since badges and
 * notes are indexed by it: card apps are hidden while the card is out, and
 * an uninstalled app's slot is emptied and reused.
 */

#include "sb_internal.h"

#include <glaze/fs.h>
#include <glaze/pkg.h>
#include <glaze/sys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ICON_MAX    (16 * 1024)

static void *icon_data[SB_MAX_APPS];
static bool seen[SB_MAX_APPS];
static bool changed;
static uint32_t reinstalled_mask;

static bool on_card(int app)
{
    return !strncmp(sb_apps[app].path, "/Card/", 6);
}

static void load_icon(int app)
{
    char path[FS_PATH_MAX];
    struct fs_stat st;
    gfx_image img;

    free(icon_data[app]);
    icon_data[app] = NULL;
    snprintf(path, sizeof(path), "%s/res/icon.gzi", sb_apps[app].path);
    int n = fs_stat(path, &st);
    if (!n && st.size && st.size <= ICON_MAX) {
        icon_data[app] = malloc(st.size);
        n = icon_data[app] ? fs_read_file(path, icon_data[app], st.size) : GZ_ENOMEM;
    } else if (!n) {
        n = GZ_EINVAL;
    }
    if (n > 0 && gfx_image_parse(&img, icon_data[app], (size_t)n)) {
        sb_set_icon(app, &img);
        return;
    }
    free(icon_data[app]);
    icon_data[app] = NULL;
    sb_set_icon(app, NULL);
}

static void found(const char *bundle, const struct pkg_info *info, void *arg)
{
    int slot = -1, free_slot = -1;

    (void)arg;
    for (int i = 0; i < sb_num_apps; i++) {
        if (!strcmp(sb_apps[i].path, bundle))
            slot = i;
        else if (sb_apps[i].installed && !sb_apps[i].path[0] && free_slot < 0)
            free_slot = i;
    }
    if (slot >= 0) {
        seen[slot] = true;
        if (!sb_apps[slot].present || strcmp(sb_apps[slot].label, info->name) ||
            strcmp(sb_apps[slot].exec, info->exec) || strcmp(sb_apps[slot].version, info->version)) {
            strlcpy(sb_apps[slot].label, info->name, sizeof(sb_apps[slot].label));
            strlcpy(sb_apps[slot].exec, info->exec, sizeof(sb_apps[slot].exec));
            strlcpy(sb_apps[slot].version, info->version, sizeof(sb_apps[slot].version));
            sb_apps[slot].present = true;
            load_icon(slot);                    /* reinstalled or the card is back */
            reinstalled_mask |= 1u << slot;
            changed = true;
        }
        return;
    }
    if (free_slot < 0) {
        if (sb_num_apps == SB_MAX_APPS)
            return;
        free_slot = sb_num_apps++;
    }
    struct sb_app_info *a = &sb_apps[free_slot];
    memset(a, 0, sizeof(*a));
    strlcpy(a->label, info->name, sizeof(a->label));
    strlcpy(a->path, bundle, sizeof(a->path));
    strlcpy(a->exec, info->exec, sizeof(a->exec));
    strlcpy(a->id, info->id, sizeof(a->id));
    strlcpy(a->version, info->version, sizeof(a->version));
    a->status_style = SB_STATUS_GRAY;
    a->installed = a->present = true;
    seen[free_slot] = true;
    load_icon(free_slot);
    notify_attach(free_slot);
    changed = true;
}

/* card_only: a cheap poll of the card while the home screen is idle. */
void apps_scan(bool card_only)
{
    memset(seen, 0, sizeof(seen));
    if (!card_only)
        pkg_list("", found, NULL);
    pkg_list("/Card", found, NULL);
    for (int i = 0; i < sb_num_apps; i++) {
        struct sb_app_info *a = &sb_apps[i];
        if (a->wince || !a->installed || !a->path[0] || seen[i] || (card_only && !on_card(i)))
            continue;
        if (on_card(i)) {
            if (a->present) {
                a->present = false;             /* the card is out */
                reinstalled_mask |= 1u << i;
                changed = true;
            }
        } else {
            notify_forget(i);                   /* deleted behind our back */
            a->path[0] = 0;
            a->present = false;
            reinstalled_mask |= 1u << i;
            changed = true;
        }
    }
}

bool apps_changed(void)
{
    bool c = changed;

    changed = false;
    return c;
}

uint32_t apps_reinstalled_mask(void)
{
    uint32_t m = reinstalled_mask;

    reinstalled_mask = 0;
    return m;
}

/* A direct CE app uses the built-in runner and a small writable context.
 * No executable, DLL or data file is copied onto the original NAND. */
int apps_wince(const char *source)
{
    if(!source||strncmp(source,"/NAND/",6)||strlen(source)>=FS_PATH_MAX)return GZ_EINVAL;
    for(const char *p=source+6;*p;){
        const char *end=strchr(p,'/');if(!end)end=p+strlen(p);
        if(end==p||(end-p==1&&*p=='.')||(end-p==2&&p[0]=='.'&&p[1]=='.')||memchr(p,':',end-p)||memchr(p,'\\',end-p))return GZ_EINVAL;
        p=*end?end+1:end;
    }
    struct fs_stat st;int rv=fs_stat(source,&st);if(rv)return rv;
    if(st.type!=FS_FILE||!st.size||st.size>4u*1024u*1024u)return GZ_EINVAL;
    uint32_t runner_bytes;if(!gz_romfs_map("bin/wince",&runner_bytes))return GZ_ENOENT;
    uint32_t hash=2166136261u;
    for(const char *p=source;*p;p++)hash=(hash^(uint8_t)*p)*16777619u;
    char context[48],path[FS_PATH_MAX],existing[FS_PATH_MAX];
    bool found=false;
    for(unsigned collision=0;collision<32;collision++){
        snprintf(context,sizeof context,"/Library/WinCE/%08x-%u.app",hash,collision);
        snprintf(path,sizeof path,"%s/source",context);
        int n=fs_read_file(path,existing,sizeof existing-1);
        if(n>=0){existing[n]=0;if(!strcmp(existing,source)){found=true;break;}continue;}
        if(n!=GZ_ENOENT)return n;
        rv=fs_mkdirs(context);if(rv)return rv;
        char tmp[FS_PATH_MAX];snprintf(tmp,sizeof tmp,"%s/source.tmp",context);
        rv=fs_write_file(tmp,source,strlen(source));if(rv!=(int)strlen(source)){fs_remove(tmp);return rv<0?rv:GZ_EIO;}
        rv=fs_rename(tmp,path);if(rv){fs_remove(tmp);return rv;}
        found=true;break;
    }
    if(!found)return GZ_ENOSPC;
    for(int i=0;i<sb_num_apps;i++)if(sb_apps[i].wince&&!strcmp(sb_apps[i].path,context))return i;
    int index=-1;
    for(int i=0;i<sb_num_apps;i++)if(sb_apps[i].installed&&!sb_apps[i].path[0]){index=i;break;}
    if(index<0){if(sb_num_apps==SB_MAX_APPS)return GZ_ENOSPC;index=sb_num_apps++;}
    struct sb_app_info *a=&sb_apps[index];memset(a,0,sizeof *a);
    strlcpy(a->path,context,sizeof a->path);snprintf(a->id,sizeof a->id,"wince.%08x",hash);
    char label[FS_NAME_MAX];const char *leaf=strrchr(source,'/');strlcpy(label,leaf?leaf+1:source,sizeof label);
    size_t len=strlen(label);if(len>4&&!strcasecmp(label+len-4,".cpk"))label[len-4]=0;
    strlcpy(a->label,label,sizeof a->label);strlcpy(a->exec,"wince",sizeof a->exec);
    a->installed=a->present=a->wince=true;a->status_style=SB_STATUS_FULLSCREEN;
    sb_set_icon(index,NULL);changed=true;return index;
}

int apps_launch(int app)
{
    struct sb_app_info *a = &sb_apps[app];
    if(a->wince)return gz_spawn("bin/wince",GZ_SPAWN_APP);
    char path[FS_PATH_MAX];
    struct fs_stat st;

    snprintf(path, sizeof(path), "%s/%s", a->path, a->exec);
    int rv = fs_stat(path, &st);
    if (rv)
        return rv;
    if (st.type != FS_FILE || !st.size || st.size > GZ_ELF_MAX_SIZE)
        return GZ_EINVAL;
    void *elf = malloc(st.size);
    if (!elf)
        return GZ_ENOMEM;
    rv = fs_read_file(path, elf, st.size);
    if (rv == (int)st.size)
        rv = gz_spawn_image(elf, st.size, a->id);
    else if (rv >= 0)
        rv = GZ_EIO;
    free(elf);
    if (rv > 0)
        a->install_id = pkg_install_id(a->path);
    return rv;
}

/* A reinstall bumps the bundle's install id (glaze/pkg.h), even at the
 * same version, so a copy still running is out of date. */
bool apps_replaced(int app)
{
    struct sb_app_info *a = &sb_apps[app];

    return a->installed && !a->wince && pkg_install_id(a->path) != a->install_id;
}

int apps_uninstall(int app)
{
    struct sb_app_info *a = &sb_apps[app];

    if (!a->installed)
        return GZ_EPERM;
    int rv = pkg_uninstall(a->path);
    if (rv && rv != GZ_ENOENT)
        return rv;
    notify_forget(app);
    free(icon_data[app]);
    icon_data[app] = NULL;
    sb_set_icon(app, NULL);
    a->path[0] = 0;
    a->present = false;
    reinstalled_mask |= 1u << app;
    changed = true;
    return 0;
}
