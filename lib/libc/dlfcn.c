/* One runtime interface and one allocator/filesystem client per process. */
#include <dlfcn.h>
#include <glaze/loader.h>
#include <glaze/thread.h>
#include <glaze/fs.h>
#include <stdlib.h>
#include <string.h>
static const struct gz_ld_api *runtime;
static int read_file(const char *path, void **bytes, uint32_t *size)
{
    struct fs_stat st;
    int rv = fs_stat(path, &st);
    if (rv) return rv;
    if (st.type != FS_FILE || !st.size || st.size > GZ_ELF_MAX_SIZE) return GZ_EINVAL;
    void *data = malloc(st.size);
    if (!data) return GZ_ENOMEM;
    int n = fs_read_file(path, data, st.size);
    if (n != (int)st.size) { free(data); return n < 0 ? n : GZ_EIO; }
    *bytes = data; *size = st.size; return 0;
}
static const struct gz_ld_hooks hooks = { read_file, free };
void __glaze_ld_init(const struct gz_ld_api *api, const struct gz_ld_hooks **out)
{ runtime = api; *out = &hooks; }
void __glaze_ld_finish(void) { if (runtime) runtime->finish(); }
static void unsupported(void)
{
    struct gz_tcb *t = gz_self();
    strlcpy(t->dl_error, "dlopen requires a dynamically linked Glaze process", sizeof(t->dl_error));
    t->dl_pending = true;
}
void *dlopen(const char *path, int mode) { if (runtime) return runtime->open(path, mode); unsupported(); return NULL; }
void *dlsym(void *handle, const char *name) { if (runtime) return runtime->symbol(handle, name); unsupported(); return NULL; }
int dlclose(void *handle) { if (runtime) return runtime->close(handle); unsupported(); return -1; }
char *dlerror(void)
{
    if (runtime) return runtime->error();
    struct gz_tcb *t = gz_self();
    if (!t->dl_pending) return NULL;
    t->dl_pending = false; return t->dl_error;
}
