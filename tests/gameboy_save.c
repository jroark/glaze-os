/* Exercise real persistence code against a filesystem with injected failures. */
#include "../packages/gameboy/save.h"
#include <glaze/fs.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct { char name[FS_PATH_MAX]; unsigned size; uint8_t data[131072]; } files[2];
static bool partial,block_final_rename;
static int find(const char *s) { for(int i=0;i<2;i++) if (!strcmp(files[i].name,s)) return i; return -1; }
int fs_stat(const char *s,struct fs_stat *st) {
    int i=find(s); if (i<0) return GZ_ENOENT; st->type=FS_FILE; st->size=files[i].size; return 0;
}
int fs_read_file(const char *s,void *p,uint32_t size) {
    int i=find(s); if(i<0) return GZ_ENOENT;
    if (size>files[i].size) size=files[i].size;
    memcpy(p,files[i].data,size); return size;
}
int fs_write_file(const char *s,const void *p,uint32_t size) {
    int i=find(s); if(i<0) for(int n=0;n<2;n++) if(!*files[n].name) { i=n; break; }
    assert(i>=0 && size<=131072);
    snprintf(files[i].name,sizeof files[i].name,"%s",s);
    if (partial && size) size--;
    memcpy(files[i].data,p,size); files[i].size=size; return size;
}
int fs_remove(const char *s) { int i=find(s); if(i<0) return GZ_ENOENT; files[i].name[0]=0; return 0; }
int fs_rename(const char *from,const char *to) {
    int i=find(from); if(i<0) return GZ_ENOENT;
    if(find(to)>=0) return GZ_EEXIST; /* FAT replacement behavior */
    if(block_final_rename) return GZ_EIO;
    snprintf(files[i].name,sizeof files[i].name,"%s",to); return 0;
}
int main(void) {
    static uint8_t data[131072],readback[131072];
    memset(data,0x51,sizeof data);
    assert(gb_save_write("/save",data,sizeof data)==0);
    assert(gb_save_read("/save",readback,sizeof readback)==sizeof readback);
    assert(!memcmp(data,readback,sizeof data));
    partial=true; memset(data,0x62,sizeof data);
    assert(gb_save_write("/save",data,sizeof data)<0);
    assert(gb_save_read("/save",readback,sizeof readback)==sizeof readback && readback[0]==0x51);
    partial=false; block_final_rename=true;
    assert(gb_save_write("/save",data,sizeof data)<0); /* completed tmp, old destination removed */
    assert(find("/save")<0 && find("/save.tmp")>=0);
    assert(gb_save_read("/save",readback,sizeof readback)==sizeof readback && readback[0]==0x62);
    block_final_rename=false;
    assert(gb_save_write("/save",data,sizeof data)==0);
    assert(gb_save_read("/save",readback,512)<0); /* never import a truncated/wrong-size save */
    char too_long[FS_PATH_MAX]; memset(too_long,'x',sizeof too_long-1); too_long[sizeof too_long-1]=0;
    assert(gb_save_write(too_long,data,1)<0);
    puts("Game Boy save PASS: 128 KiB roundtrip, partial-write preservation, FAT interrupted replacement recovery, size/path checks");
    return 0;
}
