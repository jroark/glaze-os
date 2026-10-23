#ifndef GLAZE_SEGA_SAVE_H
#define GLAZE_SEGA_SAVE_H
#include <stdint.h>
/* Uses Glaze fs_*; tests provide an in-memory filesystem backend. */
int sega_save_write(const char *path,const void *data,uint32_t size);
int sega_save_read(const char *path,void *data,uint32_t size);
#endif
