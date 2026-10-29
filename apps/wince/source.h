#ifndef WINCE_SOURCE_H
#define WINCE_SOURCE_H
#include <glaze/fs.h>
bool ce_source_init(void);
bool ce_source_active(void);
const char *ce_source_name(void);
const char *ce_source_module_name(const char *);
/* Native module/file readers return owned decoded buffers. */
void *ce_source_program(uint32_t *);
void *ce_source_library(const char *, uint32_t *);
const char *ce_source_ordinals(uint32_t *);
bool ce_source_file(const char *, char [FS_PATH_MAX]);
bool ce_source_directory(const char *, char [FS_PATH_MAX]);
#endif
