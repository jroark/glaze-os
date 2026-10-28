/* SPDX-License-Identifier: GPL-3.0-or-later
 * Buffered stdio subset for the engine. Saves serialize one byte at a time;
 * buffering keeps those writes from turning into thousands of IPC calls. */
#include <glaze/fs.h>
#include <limits.h>
#define IO_BUF 4096
struct doom_file {
    int fd, writing, error;
    uint32_t pos, start;
    unsigned used, cursor;
    uint8_t buf[IO_BUF];
};
char doom_data_dir[128];

int doom_fflush(struct doom_file *f)
{
    if (!f) return 0;
    if (f->writing && f->used) {
        unsigned done = 0;
        while (done < f->used) {
            int n = fs_write(f->fd, f->buf + done, f->used - done);
            if (n <= 0) { f->error = 1; return -1; }
            done += n;
        }
        f->used = 0;
    }
    return f->error ? -1 : 0;
}
struct doom_file *doom_fopen(const char *path, const char *mode)
{
    int writing = mode && mode[0] == 'w';
    if (!mode || (!writing && mode[0] != 'r') || strchr(mode,'+')) return NULL;
    struct doom_file *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->fd = fs_open(path, writing ? FS_O_WRITE|FS_O_CREATE|FS_O_TRUNC : FS_O_READ);
    if (f->fd < 0) { free(f); errno = ENOENT; return NULL; }
    f->writing = writing;
    return f;
}
int doom_fclose(struct doom_file *f)
{
    if (!f) return -1;
    int rv = doom_fflush(f);
    if (fs_close(f->fd) < 0) rv = -1;
    free(f);
    return rv;
}
size_t doom_fread(void *dst, size_t size, size_t count, struct doom_file *f)
{
    if (!f || f->writing || !size || count > SIZE_MAX/size) return 0;
    size_t done=0, len=size*count;
    while (done < len) {
        if (f->cursor == f->used) {
            int n=fs_read(f->fd,f->buf,IO_BUF);
            if (n<=0) { if (n<0) f->error=1; break; }
            f->start=f->pos; f->used=n; f->cursor=0;
        }
        size_t n=f->used-f->cursor;
        if (n>len-done) n=len-done;
        memcpy((uint8_t *)dst+done,f->buf+f->cursor,n);
        f->cursor+=n; f->pos+=n; done+=n;
    }
    return done/size;
}
size_t doom_fwrite(const void *src, size_t size, size_t count, struct doom_file *f)
{
    if (!f || !f->writing || f->error || !size || count>SIZE_MAX/size) return 0;
    size_t done=0, len=size*count;
    while (done<len) {
        if (f->used==IO_BUF && doom_fflush(f)<0) break;
        size_t n=IO_BUF-f->used;
        if (n>len-done) n=len-done;
        memcpy(f->buf+f->used,(const uint8_t *)src+done,n);
        f->used+=n; f->pos+=n; done+=n;
    }
    return done/size;
}
long doom_ftell(struct doom_file *f) { return f ? (long)f->pos : -1; }
int doom_fseek(struct doom_file *f, long offset, int whence)
{
    if (!f || offset<INT_MIN || offset>INT_MAX || doom_fflush(f)<0) return -1;
    /* A read buffer may have advanced the underlying descriptor. */
    int64_t target=offset;
    if (whence==SEEK_CUR) { target+=f->pos; whence=SEEK_SET; }
    if (target<INT_MIN || target>INT_MAX) return -1;
    if (!f->writing && whence==SEEK_SET && target>=f->start && target<=(int64_t)f->start+f->used) {
        f->cursor=(unsigned)(target-f->start); f->pos=(uint32_t)target; return 0;
    }
    int p=fs_seek(f->fd,(int32_t)target,whence);
    if (p<0) { f->error=1; return -1; }
    f->pos=f->start=p; f->used=f->cursor=0;
    return 0;
}
int doom_vfprintf(struct doom_file *f, const char *fmt, va_list ap)
{
    char buf[1024];
    int n=vsnprintf(buf,sizeof buf,fmt,ap);
    if (n<0 || n>=(int)sizeof buf) return -1;
    if (!f) { printf("%s",buf); return n; }
    return doom_fwrite(buf,1,n,f)==(size_t)n ? n : -1;
}
int doom_fprintf(struct doom_file *f, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt); int n=doom_vfprintf(f,fmt,ap); va_end(ap); return n;
}
int doom_mkdir(const char *path, int mode) { (void)mode; return fs_mkdirs(path)<0 ? -1 : 0; }
int doom_remove(const char *path) { return fs_remove(path)<0 ? -1 : 0; }
int doom_rename(const char *from, const char *to)
{
    int rv=fs_rename(from,to);
    /* FAT cannot replace; the complete temporary file survives a failed
     * second rename and can be recovered from the app data folder. */
    if (rv==GZ_EEXIST && fs_remove(to)>=0) rv=fs_rename(from,to);
    return rv<0 ? -1 : 0;
}
char *doom_getenv(const char *name) { (void)name; return NULL; }
