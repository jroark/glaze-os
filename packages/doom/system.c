/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "port.h"
#include "i_system.h"
#include "i_sound.h"
#include <glaze/audio.h>
static struct { atexit_func_t func; boolean on_error; } hooks[16];
static int hook_count;
void I_AtExit(atexit_func_t f, boolean error)
{
    if (hook_count==16) I_Error("Too many exit callbacks");
    hooks[hook_count].func=f; hooks[hook_count++].on_error=error;
}
void I_Quit(void)
{
    puts("doom: quit");
    for (int i=hook_count-1;i>=0;i--) hooks[i].func();
    audio_stream_close();
    exit(0);
}
void I_Error(char *fmt, ...)
{
    char msg[192]; va_list ap; va_start(ap,fmt); vsnprintf(msg,sizeof msg,fmt,ap); va_end(ap);
    printf("doom: error: %s\n",msg);
    /* No save/config callbacks after a fatal engine error. */
    audio_stream_close(); doom_failure(msg);
}
byte *I_ZoneBase(int *size)
{
    /* Fit alongside SpringBoard, FS and audio in 16 MiB. WADs stay on disk. */
    *size=4*1024*1024;
    byte *mem=malloc(*size);
    if (!mem) I_Error("Not enough memory. Close other apps and retry.");
    return mem;
}
boolean I_ConsoleStdout(void) { return false; }
void I_PrintBanner(char *s) { puts(s); }
void I_PrintDivider(void) { puts("--------------------------------"); }
void I_PrintStartupBanner(char *s) { puts(s); puts("DoomGeneric / Glaze; GPL, no warranty."); }
void I_Tactile(int on,int off,int total) { (void)on; (void)off; (void)total; }
boolean I_GetMemoryValue(unsigned offset,void *value,int size)
{
    static const byte dos[10]={0x57,0x92,0x19,0,0xf4,6,0x70,0,0x16,0};
    if (size!=1 && size!=2 && size!=4) return false;
    if (offset>sizeof dos-(unsigned)size) return false;
    unsigned v=0; for (int i=0;i<size;i++) v|=(unsigned)dos[offset+i]<<(8*i);
    if (size==1) *(uint8_t *)value=v;
    else if (size==2) *(uint16_t *)value=v;
    else *(uint32_t *)value=v;
    return true;
}
