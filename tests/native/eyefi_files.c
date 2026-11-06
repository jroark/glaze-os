/* Exercise the actual application's filesystem adapter, including failure
 * before the request commit and cancellation when existing handles go stale. */
#include <glaze/fs.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GLAZE_SYS_H
void gz_sleep_ms(uint32_t ms);
#define main eyefi_app_main
#include "../../apps/eyefi/main.c"
#undef main

static unsigned closed, writes, sleeps, missing, short_write, gone, busy;
static unsigned creates, creating, bad_size, close_error;
static unsigned scripted, renamed, research_runs;
int fs_read_file(const char *path, void *buf, uint32_t max)
{
    assert(!strcmp(path, SCRIPT_PATH) && max == sizeof(script) - 1);
    if (!scripted) return GZ_ENOENT;
    memcpy(buf, "info 1\n", 7); return 7;
}
int fs_remove(const char *path) { assert(!strcmp(path, SCRIPT_DONE)); return GZ_ENOENT; }
int fs_rename(const char *from, const char *to)
{
    assert(!strcmp(from, SCRIPT_PATH) && !strcmp(to, SCRIPT_DONE)); ++renamed; return 0;
}
int eyefi_research(const struct eyefi_io *io, uint8_t *buf, char *s)
{
    (void)io; (void)buf;
    assert(renamed == 1 && !strcmp(s, "info 1\n")); ++research_runs; return 0;
}
static uint8_t mail[4][EYEFI_SIZE];
static unsigned pos[4];
int fs_stat(const char *path, struct fs_stat *st)
{
    if (!strcmp(path,"/Card/EyeFi")) { st->type=FS_DIR; st->size=0; return 0; }
    for (unsigned i=0;i<4;++i) if (!strcmp(path,paths[i])) {
        if (missing & (1u<<i)) return GZ_ENOENT;
        st->type=FS_FILE; st->size=bad_size & (1u<<i) ? 512 : EYEFI_SIZE; return 0;
    }
    assert(0); return GZ_ENOENT;
}
int fs_mkdir(const char *path) { assert(!strcmp(path,"/Card/EyeFi")); return GZ_EEXIST; }
int fs_open(const char *path, int flags)
{
    static const char *names[]={"reqm","reqc","rspm","rspc"};
    if (busy) return GZ_EBUSY;
    for (int i=0;i<4;++i) if (strstr(path,names[i])) {
        if (flags & FS_O_CREATE) {
            assert(flags==(FS_O_WRITE|FS_O_CREATE|FS_O_EXCL));
            assert(missing & (1u<<i)); ++creates; creating |= 1u<<i;
            memset(mail[i],0xab,EYEFI_SIZE);
        } else assert(flags==(FS_O_READ | (i<2 ? FS_O_WRITE : 0)));
        pos[i]=0; return i;
    }
    assert(0); return -1;
}
int fs_seek(int fd, int32_t off, int whence)
{
    assert(fd>=0 && fd<4 && !off && whence==FS_SEEK_SET);
    if (gone) return GZ_EIO;
    pos[fd]=0; return 0;
}
int fs_read(int fd, void *buf, uint32_t len)
{
    if (gone) return GZ_EIO;
    assert(fd>=2 && len==FS_IO_MAX && pos[fd]+len<=EYEFI_SIZE);
    memcpy(buf,mail[fd]+pos[fd],len); pos[fd]+=len; return (int)len;
}
int fs_write(int fd, const void *buf, uint32_t len)
{
    assert((fd<2 || (creating & (1u<<fd))) && len==FS_IO_MAX && pos[fd]+len<=EYEFI_SIZE);
    ++writes;
    if (short_write) return 512;
    memcpy(mail[fd]+pos[fd],buf,len); pos[fd]+=len;
    if (!(creating & (1u<<fd)) && fd==EYEFI_REQC && pos[fd]==EYEFI_SIZE && !sleeps) {
        memcpy(mail[EYEFI_RSPC],mail[EYEFI_REQC],EYEFI_SIZE);
        unsigned q=mail[EYEFI_REQM][1];
        mail[EYEFI_RSPM][0]=q==1 ? 6 : q==2 ? 4 : q==13 ? 3 : 1;
    }
    return (int)len;
}
int fs_close(int fd) {
    assert(fd>=0 && fd<4); closed |= 1u<<fd;
    if (creating & (1u<<fd)) {
        creating &= ~(1u<<fd); missing &= ~(1u<<fd);
        if(pos[fd]!=EYEFI_SIZE) bad_size |= 1u<<fd;
        else for(unsigned i=0;i<EYEFI_SIZE;++i) assert(!mail[fd][i]);
    }
    return close_error ? GZ_EIO : 0;
}
void gz_sleep_ms(uint32_t ms) { assert(ms==300); if (sleeps) gone=1; }
static void reset(void)
{
    closed=writes=sleeps=missing=short_write=gone=busy=0;
    creates=creating=bad_size=close_error=0;
    scripted=renamed=research_runs=0;
    memset(mail,0,sizeof(mail));
    for (unsigned i=0;i<4;++i) handles[i]=-1;
}
int main(void)
{
    reset(); assert(eyefi_app_main()==0 && closed==15 && writes==40);
    reset(); missing=15; assert(eyefi_app_main()==0 && creates==4 && writes==56);
    reset(); missing=4; assert(eyefi_app_main()==0 && creates==1 && writes==44);
    reset(); bad_size=8; missing=1; assert(eyefi_app_main()==1 && !creates && !writes);
    reset(); missing=15; short_write=1;
    assert(eyefi_app_main()==1 && creates==1 && writes==1 && bad_size==1);
    reset(); missing=15; close_error=1;
    assert(eyefi_app_main()==1 && creates==1 && writes==4);
    reset(); busy=1; assert(eyefi_app_main()==1 && !writes && !closed);
    reset(); short_write=1; assert(eyefi_app_main()==1 && writes==1 && closed==15);
    reset(); sleeps=1; assert(eyefi_app_main()==1 && gone && closed==15);
    reset(); scripted=1;
    assert(eyefi_app_main()==0 && research_runs==1 && !writes && closed==15);
    puts("eyefi: filesystem adapter PASS");
}
