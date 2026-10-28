/* Private stdio bridge; never changes Glaze's socket descriptor ABI. */
#ifndef GLAZE_DOOM_COMPAT_H
#define GLAZE_DOOM_COMPAT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
struct doom_file;
struct doom_file *doom_fopen(const char *, const char *);
int doom_fclose(struct doom_file *);
size_t doom_fread(void *, size_t, size_t, struct doom_file *);
size_t doom_fwrite(const void *, size_t, size_t, struct doom_file *);
long doom_ftell(struct doom_file *);
int doom_fseek(struct doom_file *, long, int);
int doom_fflush(struct doom_file *);
int doom_fprintf(struct doom_file *, const char *, ...) __attribute__((format(printf,2,3)));
int doom_vfprintf(struct doom_file *, const char *, va_list);
int doom_mkdir(const char *, int);
int doom_remove(const char *);
int doom_rename(const char *, const char *);
char *doom_getenv(const char *);
extern char doom_data_dir[128];
#ifndef DOOM_COMPAT_IMPLEMENTATION
#define FILE struct doom_file
#define fopen doom_fopen
#define fclose doom_fclose
#define fread doom_fread
#define fwrite doom_fwrite
#define ftell doom_ftell
#define fseek doom_fseek
#define fflush doom_fflush
#define fprintf doom_fprintf
#define vfprintf doom_vfprintf
#define mkdir doom_mkdir
#define remove doom_remove
#define rename doom_rename
#define getenv doom_getenv
#undef stdout
#undef stderr
#define stdout ((struct doom_file *)0)
#define stderr ((struct doom_file *)0)
#endif
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#ifndef EISDIR
#define EISDIR 21
#endif
#endif
