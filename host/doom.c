/* Native checks of the actual port I/O, WAD validation and engine. */
#define DOOM_COMPAT_IMPLEMENTATION
#include "../packages/doom/compat.h"
#include "../packages/doom/port.h"
#include "doomgeneric.h"
#include "doomstat.h"
#include "doomkeys.h"
#include "g_game.h"
#include "p_saveg.h"
#include "i_sound.h"
#include "i_system.h"
#include <glaze/fs.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
static FILE *files[32];
static int fail_write, short_io, reads,writes;
static uint32_t milliseconds, frames, goal=120, tick, nonzero, checksum;
static int16_t pcm_copy[8192];
static unsigned pcm_frames;
int fs_open(const char *path,int flags)
{
    for (int i=1;i<32;i++) if (!files[i]) {
        files[i]=fopen(path,flags&FS_O_WRITE ? "wb" : "rb"); return files[i] ? i : GZ_ENOENT;
    }
    return GZ_ENOSPC;
}
int fs_read(int fd,void *p,uint32_t len)
{
    reads++; if (short_io && len>7) len=7;
    size_t n=fread(p,1,len,files[fd]); return ferror(files[fd]) ? GZ_EIO : (int)n;
}
int fs_write(int fd,const void *p,uint32_t len)
{
    writes++; if (fail_write) return GZ_EIO;
    if (short_io && len>7) len=7;
    size_t n=fwrite(p,1,len,files[fd]); return ferror(files[fd]) ? GZ_EIO : (int)n;
}
int fs_close(int fd) { int n=fclose(files[fd]); files[fd]=NULL; return n ? GZ_EIO : 0; }
int fs_seek(int fd,int32_t off,int whence)
{ if (fseek(files[fd],off,whence)) return GZ_EIO; return (int)ftell(files[fd]); }
int fs_mkdirs(const char *p) { return mkdir(p,0700)==0 ? 0 : errno==EEXIST ? 0 : GZ_EIO; }
int fs_remove(const char *p) { return remove(p)==0 ? 0 : GZ_EIO; }
int fs_rename(const char *a,const char *b) { return rename(a,b)==0 ? 0 : GZ_EIO; }
int audio_stream_open(uint32_t rate,int channels) { assert(rate==11025 && channels==1); return 0; }
int audio_stream_close(void) { return 0; }
int audio_stream_clock(uint32_t *played,uint32_t *queued) { *played=milliseconds; *queued=0; return 0; }
int audio_stream_write(const void *p,uint32_t len)
{
    assert(len<=sizeof pcm_copy);
    if (len) { memcpy(pcm_copy,p,len); pcm_frames+=len/2; for (unsigned i=0;i<len/2;i++) if (((const int16_t *)p)[i]) nonzero++; }
    return (int)len;
}
void doom_pump(void) {}
void doom_failure(const char *msg) { fprintf(stderr,"doom failure: %s\n",msg); exit(1); }
void DG_Init(void) {}
void DG_DrawFrame(void) {}
void DG_SetWindowTitle(const char *s) { printf("title: %s\n",s); }
uint32_t DG_GetTicksMs(void) { return milliseconds; }
void DG_SleepMs(uint32_t ms) { milliseconds+=ms ? ms : 1; }
static int previous[256];
int DG_GetKey(int *pressed,unsigned char *key)
{
    for (unsigned i=1;i<256;i++) {
        int wanted=(i==KEY_UPARROW && tick>=5 && tick<30) || (i==KEY_FIRE && tick>=15 && tick<25);
        if (previous[i]!=wanted) { previous[i]=wanted; *key=i; *pressed=wanted; return 1; }
    }
    return 0;
}
void doom_present(const uint8_t *p,const uint16_t *pal)
{
    uint32_t h=2166136261u;
    for (unsigned i=0;i<64000;i++) h=(h^pal[p[i]])*16777619u;
    checksum^=h; milliseconds+=29; frames++;
}
static void self_test(void)
{
    char root[]="/tmp/glaze-doom-XXXXXX"; assert(mkdtemp(root));
    char path[256]; snprintf(path,sizeof path,"%s/data",root);
    unsigned char data[10003],copy[10003]; for (unsigned i=0;i<sizeof data;i++) data[i]=(unsigned char)(i*17);
    struct doom_file *f=doom_fopen(path,"wb"); assert(f);
    for (unsigned i=0;i<sizeof data;i++) assert(doom_fwrite(data+i,1,1,f)==1);
    assert(doom_ftell(f)==sizeof data && doom_fclose(f)==0 && writes==3);
    f=doom_fopen(path,"rb"); assert(f); reads=0;
    assert(doom_fread(copy,1,3000,f)==3000); assert(!memcmp(copy,data,3000));
    assert(doom_fseek(f,-100,SEEK_CUR)==0 && doom_ftell(f)==2900);
    assert(doom_fread(copy,1,100,f)==100 && !memcmp(copy,data+2900,100) && reads==1);
    assert(doom_fseek(f,-3,SEEK_END)==0);
    assert(doom_fread(copy,1,10,f)==3 && !memcmp(copy,data+10000,3));
    assert(doom_fread(copy,SIZE_MAX,2,f)==0); assert(doom_fclose(f)==0);
    short_io=1; f=doom_fopen(path,"wb"); assert(f);
    assert(doom_fwrite(data,1,sizeof data,f)==sizeof data && doom_fclose(f)==0);
    f=doom_fopen(path,"rb"); assert(f);
    assert(doom_fread(copy,1,sizeof copy,f)==sizeof copy && !memcmp(data,copy,sizeof copy)); assert(doom_fclose(f)==0); short_io=0;
    f=doom_fopen(path,"wb"); assert(f); assert(doom_fwrite(data,1,100,f)==100); fail_write=1; assert(doom_fclose(f)<0); fail_write=0;
    /* No game assets: only a synthetic directory fixture. */
    uint8_t wad[28]={'I','W','A','D',1,0,0,0,12,0,0,0,0,0,0,0,0,0,0,0,'E','1','M','1',0,0,0,0};
    FILE *out=fopen(path,"wb"); assert(out); assert(fwrite(wad,1,sizeof wad,out)==sizeof wad); fclose(out);
    uint32_t hash; assert(!doom_check_wad(path,&hash));
    wad[4]=255; out=fopen(path,"wb"); fwrite(wad,1,sizeof wad,out); fclose(out); assert(doom_check_wad(path,&hash));
    wad[4]=1; wad[16]=255; out=fopen(path,"wb"); fwrite(wad,1,sizeof wad,out); fclose(out); assert(doom_check_wad(path,&hash));
    assert(doom_map_key(GZ_KEY_OK,0)==KEY_FIRE && doom_map_key(GZ_KEY_OK,1)==KEY_ENTER);
    assert(doom_map_key(GZ_KEY_ESC,0)==KEY_USE && doom_map_key(GZ_KEY_ESC,1)==KEY_ESCAPE);
    assert(doom_map_key(GZ_KEY_HOME,0)==0);
    /* A down event mapped as Fire must release Fire even if a menu opens. */
    int down; unsigned char key;
    doom_input_key(GZ_KEY_OK,1,0);
    assert(doom_input_next(&down,&key) && down && key==KEY_FIRE);
    doom_input_key(GZ_KEY_OK,0,1);
    assert(doom_input_next(&down,&key) && !down && key==KEY_FIRE);
    /* Press/release entirely between tics must still reach Doom. */
    doom_input_key(GZ_KEY_UP,1,0); doom_input_key(GZ_KEY_UP,0,0);
    assert(doom_input_next(&down,&key) && down && key==KEY_UPARROW);
    assert(doom_input_next(&down,&key) && !down && key==KEY_UPARROW);
    doom_input_key(GZ_KEY_OK,1,1); doom_input_key(GZ_KEY_ENTER,1,1);
    assert(doom_input_next(&down,&key) && down && key==KEY_ENTER);
    doom_input_key(GZ_KEY_OK,0,1); assert(!doom_input_next(&down,&key));
    doom_input_key(GZ_KEY_ENTER,0,1);
    assert(doom_input_next(&down,&key) && !down && key==KEY_ENTER);
    const char *name="Glaze save";
    for (unsigned i=0;name[i];i++) doom_input_text(name[i]);
    for (unsigned i=0;name[i];i++) {
        assert(doom_input_next(&down,&key) && down && key==(unsigned)name[i]);
        assert(doom_input_next(&down,&key) && !down && key==(unsigned)name[i]);
    }
    assert(!doom_input_next(&down,&key));
    doom_input_key(GZ_KEY_LEFT,1,0);
    assert(doom_input_next(&down,&key) && down && key==KEY_LEFTARROW);
    doom_input_release();
    assert(doom_input_next(&down,&key) && !down && key==KEY_LEFTARROW);
    assert(remove(path)==0 && rmdir(root)==0);
    puts("doom: buffered I/O, failure, WAD validation and button tests PASS");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    if (argc==1) { self_test(); return 0; }
    assert(argc==2);
    char dir[]="/tmp/glaze-doom-play-XXXXXX"; assert(mkdtemp(dir));
    snprintf(doom_data_dir,sizeof doom_data_dir,"%s",dir);
    uint32_t id; const char *error=doom_check_wad(argv[1],&id); if (error) { fprintf(stderr,"%s\n",error); return 1; }
    char *args[]={"doom","-iwad",argv[1],"-warp","1","1","-skill","2","-nomusic","-nogui",NULL};
    doomgeneric_Create(10,args);
    int32_t initial_x=players[0].mo->x,initial_y=players[0].mo->y,saved_x=0,saved_y=0;
    for (tick=0;tick<goal;tick++) {
        if (tick==40) { players[0].mo->momx=players[0].mo->momy=0; saved_x=players[0].mo->x; saved_y=players[0].mo->y; G_SaveGame(0,"Native test"); }
        if (tick==60) { char *s=P_SaveGameFile(0); G_LoadGame(s); }
        doomgeneric_Tick();
        if (tick==65) assert(players[0].mo->x==saved_x && players[0].mo->y==saved_y);
    }
    assert(initial_x!=saved_x || initial_y!=saved_y); assert(checksum && nonzero && pcm_frames);
    printf("doom: engine PASS: %u frames, movement, save/load, %u nonzero PCM samples, checksum %08x\n",frames,nonzero,checksum);
    I_ShutdownSound(); return 0;
}
