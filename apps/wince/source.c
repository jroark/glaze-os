/* Direct NAND applications retain their original binaries on /NAND. Mutable
 * state and decoded data files live in the private Glaze context instead. */
#include "runtime.h"
#include "source.h"
#include "assets.h"

static bool active, metadata_loaded;
static char program[FS_PATH_MAX], directory[FS_PATH_MAX], ce_name[FS_PATH_MAX+16];
static struct ce_nk_assets metadata;
static char loaded_names[16][32], loaded_paths[16][FS_PATH_MAX+16];
static unsigned loaded_count;
static uint32_t last_progress;

bool ce_source_active(void) { return active; }
const char *ce_source_name(void) { return active ? ce_name : NULL; }
static bool suffix(const char *name,const char *tail)
{
    size_t n=strlen(name),t=strlen(tail);
    return n>=t&&!strcasecmp(name+n-t,tail);
}
bool ce_source_init(void)
{
    char path[FS_PATH_MAX];
    snprintf(path,sizeof path,"%s/source",res_bundle());
    struct fs_stat st;
    if(fs_stat(path,&st))return false;
    if(st.type!=FS_FILE||!st.size||st.size>=sizeof program||fs_read_file(path,program,st.size)!=(int)st.size)
        ce_fatal("Invalid direct CE application source");
    if(memchr(program,0,st.size)||strncmp(program,"/NAND/",6))ce_fatal("Invalid direct CE application path");
    program[st.size]=0;
    /* SpringBoard has already validated components. Validate again before I/O. */
    for(const char *p=program+6;*p;){
        const char *end=strchr(p,'/');if(!end)end=p+strlen(p);
        if(end==p||(end-p==1&&p[0]=='.')||(end-p==2&&p[0]=='.'&&p[1]=='.')||memchr(p,':',end-p)||memchr(p,'\\',end-p))
            ce_fatal("Invalid direct CE application path");
        p=*end?end+1:end;
    }
    strlcpy(directory,program,sizeof directory);char *slash=strrchr(directory,'/');if(slash)*slash=0;
    snprintf(ce_name,sizeof ce_name,"\\Nand Disk%s",program+5);
    if(suffix(ce_name,".cpk"))ce_name[strlen(ce_name)-4]=0;
    for(char *p=ce_name;*p;p++)if(*p=='/')*p='\\';
    active=true;return true;
}
static int nand_read(void *arg,uint32_t offset,void *out,uint32_t size)
{ (void)arg;return fs_nand_read(offset,out,size); }
static void progress(void *arg,uint32_t done,uint32_t total)
{
    (void)arg;
    uint32_t now=gz_time_ms();if(now-last_progress<250 && done!=total)return;last_progress=now;
    gfx_fill(&ui.surf,gfx_r(0,0,240,320),0xeeeeee);
    gfx_text(&ui.surf,10,32,ui_font(FONT_B14),"Opening WinCE App",0x222222,255);
    gfx_text(&ui.surf,10,70,ui_font(FONT_R12),"Reading original CE metadata...",0x333333,255);
    char line[32];snprintf(line,sizeof line,"%u%%",total?done*100u/total:0);
    gfx_text(&ui.surf,10,100,ui_font(FONT_R12),line,0x333333,255);
    gfx_text(&ui.surf,10,145,ui_font(FONT_R11),"This runs once per RAM boot.",0x555555,255);
    ui_invalidate_all();ui_present();
}
static uint32_t hash_bytes(uint32_t hash,const void *data,uint32_t size)
{ const uint8_t *p=data;while(size--)hash=(hash^*p++)*16777619u;return hash; }
static bool cache_key(uint32_t *key)
{
    uint8_t page[512];
    if(fs_nand_read(0,page,sizeof page)!=sizeof page)return false;
    uint32_t start=ce_u32(page+40),count=ce_u32(page+44);
    if(start>=32768||!count||count>32768-start)return false;
    uint32_t h=hash_bytes(2166136261u,page,sizeof page);
    if(fs_nand_read(start*512,page,sizeof page)!=sizeof page)return false;
    h=hash_bytes(h,page,sizeof page);
    if(fs_nand_read((start+count-1)*512,page,sizeof page)!=sizeof page)return false;
    *key=hash_bytes(h,page,sizeof page);return true;
}
static void load_metadata(void)
{
    if(metadata_loaded)return;
    metadata_loaded=true;
    uint32_t key=0;cache_key(&key);
    const char *cache="/Library/WinCE/rom.cache";
    char tmp[64];snprintf(tmp,sizeof tmp,"/Library/WinCE/rom.%u.tmp",(unsigned)gz_getpid());
    uint8_t header[24];int fd=fs_open(cache,FS_O_READ);
    if(fd>=0){
        if(fs_read(fd,header,sizeof header)==sizeof header&&ce_u32(header)==0x31414357u&&ce_u32(header+4)==key&&key){
            uint32_t names=ce_u32(header+8),dict=ce_u32(header+12);
            if(names&&names<=CE_ORDINAL_MAX&&dict>=0x104&&dict<=CE_DICT_MAX){
                metadata.ordinals=malloc(names);metadata.dictionary=malloc(dict);
                if(metadata.ordinals&&metadata.dictionary&&fs_read(fd,metadata.ordinals,names)==(int)names&&
                    fs_read(fd,metadata.dictionary,dict)==(int)dict&&
                    hash_bytes(hash_bytes(2166136261u,metadata.ordinals,names),metadata.dictionary,dict)==ce_u32(header+16)){
                    metadata.ordinal_bytes=names;metadata.dictionary_bytes=dict;fs_close(fd);return;
                }
                ce_nk_assets_free(&metadata);
            }
        }
        fs_close(fd);
    }
    progress(NULL,0,1);
    const char *error=ce_nk_recover(nand_read,NULL,progress,&metadata);
    if(error)ce_fatal(error);
    if(metadata.checksum_mismatches)printf("wince: metadata recovery audited %u NK record checksum mismatches\n",metadata.checksum_mismatches);
    printf("wince: recovered %u bytes of matching CE ordinal names and %u CCFS dictionary bytes\n",
           metadata.ordinal_bytes,metadata.dictionary_bytes);
    memset(header,0,sizeof header);ce_put32(header,0x31414357u);ce_put32(header+4,key);
    ce_put32(header+8,metadata.ordinal_bytes);ce_put32(header+12,metadata.dictionary_bytes);
    ce_put32(header+16,hash_bytes(hash_bytes(2166136261u,metadata.ordinals,metadata.ordinal_bytes),metadata.dictionary,metadata.dictionary_bytes));
    fs_mkdirs("/Library/WinCE");fd=fs_open(tmp,FS_O_WRITE|FS_O_CREATE|FS_O_TRUNC);
    if(fd>=0){
        bool ok=fs_write(fd,header,sizeof header)==sizeof header&&fs_write(fd,metadata.ordinals,metadata.ordinal_bytes)==(int)metadata.ordinal_bytes&&
                fs_write(fd,metadata.dictionary,metadata.dictionary_bytes)==(int)metadata.dictionary_bytes;
        if(fs_close(fd))ok=false;
        if(!ok||fs_rename(tmp,cache))fs_remove(tmp);
    }
}
const char *ce_source_ordinals(uint32_t *size)
{
    load_metadata();*size=metadata.ordinal_bytes;return metadata.ordinals;
}
static void *read_file(const char *path,uint32_t *size)
{
    struct fs_stat st;
    if(fs_stat(path,&st)||st.type!=FS_FILE||!st.size||st.size>CE_ASSET_MAX)return NULL;
    void *data=malloc(st.size);
    if(!data||fs_read_file(path,data,st.size)!=(int)st.size){free(data);return NULL;}
    *size=st.size;
    if(st.size>=12&&!memcmp(data,"CASIO.$&%-Z\1",12)){
        /* Release NK before holding a second large executable buffer. */
        load_metadata();void *decoded=NULL;uint32_t bytes=0;
        const char *error=ce_cpk_unpack(data,st.size,metadata.dictionary,metadata.dictionary_bytes,&decoded,&bytes);
        free(data);if(error)ce_fatal(error);data=decoded;*size=bytes;
    }
    return data;
}
static bool find_file(const char *path,char out[FS_PATH_MAX])
{
    struct fs_stat st;
    if(strlen(path)>=FS_PATH_MAX)return false;
    strlcpy(out,path,FS_PATH_MAX);
    if(!fs_stat(out,&st)&&st.type==FS_FILE)return true;
    int n=snprintf(out,FS_PATH_MAX,"%s.cpk",path);
    return n>0&&n<FS_PATH_MAX&&!fs_stat(out,&st)&&st.type==FS_FILE;
}
void *ce_source_program(uint32_t *size)
{
    /* Decode metadata before reading a potentially large compressed program. */
    if(suffix(program,".cpk"))load_metadata();
    return read_file(program,size);
}
const char *ce_source_module_name(const char *name)
{
    for(unsigned i=0;i<loaded_count;i++)if(!strcasecmp(name,loaded_names[i]))return loaded_paths[i];
    return NULL;
}
void *ce_source_library(const char *name,uint32_t *size)
{
    if(!active||!name||!*name||strlen(name)>=32||strchr(name,'/')||strchr(name,'\\')||strchr(name,':')||strstr(name,".."))return NULL;
    const char *folders[]={directory,"/NAND/Windows","/NAND/Program Files"};
    for(unsigned i=0;i<ARRAY_SIZE(folders);i++){
        char path[FS_PATH_MAX],found[FS_PATH_MAX];
        int n=snprintf(path,sizeof path,"%s/%s",folders[i],name);
        if(n<0||n>=FS_PATH_MAX||!find_file(path,found))continue;
        if(suffix(found,".cpk"))load_metadata();
        void *data=read_file(found,size);if(!data)return NULL;
        if(loaded_count<ARRAY_SIZE(loaded_names)){
            strlcpy(loaded_names[loaded_count],name,sizeof loaded_names[0]);
            snprintf(loaded_paths[loaded_count],sizeof loaded_paths[0],"\\Nand Disk%s",found+5);
            char *p=loaded_paths[loaded_count];if(suffix(p,".cpk"))p[strlen(p)-4]=0;
            for(;*p;p++)if(*p=='/')*p='\\';
            loaded_count++;
        }
        return data;
    }
    return NULL;
}
/* Map CE file paths to the original volume. Writes keep using ce_path's
 * private drive; the existing copy-on-write file handling consumes this. */
static bool source_path(const char *canonical,char out[FS_PATH_MAX])
{
    if(!active||!canonical||*canonical!='/')return false;
    int n;
    if(!strncasecmp(canonical,"/Documents",10)&&(!canonical[10]||canonical[10]=='/'))
        n=snprintf(out,FS_PATH_MAX,"/NAND/My Documents%s",canonical+10);
    else if(!strcmp(canonical,"/"))n=snprintf(out,FS_PATH_MAX,"/NAND");
    else n=snprintf(out,FS_PATH_MAX,"/NAND%s",canonical);
    return n>0&&n<FS_PATH_MAX;
}
bool ce_source_directory(const char *canonical,char out[FS_PATH_MAX])
{
    struct fs_stat st;
    return source_path(canonical,out)&&!fs_stat(out,&st)&&st.type==FS_DIR;
}
bool ce_source_file(const char *canonical,char out[FS_PATH_MAX])
{
    char path[FS_PATH_MAX],found[FS_PATH_MAX];
    if(!source_path(canonical,path)||!find_file(path,found))return false;
    if(!suffix(found,".cpk")){strlcpy(out,found,FS_PATH_MAX);return true;}
    /* Compressed data needs a decoded backing file for the ordinary CE file
     * handles. Keep it in the private drive, never beside its NAND source. */
    int n=snprintf(out,FS_PATH_MAX,"%s/data/decoded%s",res_bundle(),canonical);
    if(n<0||n>=FS_PATH_MAX)return false;
    struct fs_stat st;if(!fs_stat(out,&st)&&st.type==FS_FILE)return true;
    uint32_t size=0;void *data=read_file(found,&size);if(!data)return false;
    char parent[FS_PATH_MAX];strlcpy(parent,out,sizeof parent);char *slash=strrchr(parent,'/');if(slash)*slash=0;
    bool ok=!fs_mkdirs(parent)&&fs_write_file(out,data,size)==(int)size;free(data);
    if(!ok)fs_remove(out);
    return ok;
}
