#include "save.h"
#include <glaze/fs.h>
#include <stdio.h>
static int temp_path(char out[FS_PATH_MAX],const char *path)
{
    return snprintf(out,FS_PATH_MAX,"%s.tmp",path)<FS_PATH_MAX ? 0 : GZ_EINVAL;
}
int sega_save_write(const char *path,const void *data,uint32_t size)
{
    char temp[FS_PATH_MAX];
    if (temp_path(temp,path)<0) return GZ_EINVAL;
    int rv=fs_write_file(temp,data,size);
    if (rv!=(int)size) { fs_remove(temp); return rv<0 ? rv : GZ_EIO; }
    rv=fs_rename(temp,path);
    if (rv==GZ_EEXIST && fs_remove(path)>=0) rv=fs_rename(temp,path);
    return rv;
}
static int read_exact(const char *path,void *data,uint32_t size)
{
    struct fs_stat st;
    int rv=fs_stat(path,&st);
    if (rv<0) return rv;
    if (st.type!=FS_FILE || st.size!=size) return GZ_EIO;
    rv=fs_read_file(path,data,size);
    return rv==(int)size ? rv : GZ_EIO;
}
int sega_save_read(const char *path,void *data,uint32_t size)
{
    int rv=read_exact(path,data,size);
    if (rv==(int)size) return rv;
    char temp[FS_PATH_MAX];
    if (temp_path(temp,path)<0) return GZ_EINVAL;
    int recovery=read_exact(temp,data,size);
    return recovery==GZ_ENOENT && rv!=GZ_ENOENT ? rv : recovery;
}
