#ifndef GLAZE_GAMEBOY_SAVE_H
#define GLAZE_GAMEBOY_SAVE_H
#include <stdint.h>
/* Uses Glaze fs_*; tests provide an in-memory filesystem backend. */
int gb_save_write(const char *path,const void *data,uint32_t size);
int gb_save_read(const char *path,void *data,uint32_t size);
#endif
