/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "port.h"
#include "doomgeneric.h"
#include "i_video.h"
#include "v_video.h"
#include "tables.h"
#include "z_zone.h"
#include <limits.h>
boolean screenvisible=true, screensaver_mode=false;
byte *I_VideoBuffer;
int usegamma;
float mouse_acceleration=1.0f;
int mouse_threshold=10;
static uint16_t palette[256];
static uint8_t rgb[768];
void I_InitGraphics(void)
{
    I_VideoBuffer=Z_Malloc(SCREENWIDTH*SCREENHEIGHT,PU_STATIC,NULL);
    puts("doom: graphics ready (320x200 -> 240x180)");
}
void I_ShutdownGraphics(void) { if (I_VideoBuffer) Z_Free(I_VideoBuffer); I_VideoBuffer=NULL; }
void I_StartFrame(void) { doom_pump(); }
void I_StartTic(void) { extern void I_GetEvent(void); I_GetEvent(); }
void I_FinishUpdate(void) { doom_present(I_VideoBuffer,palette); }
void I_ReadScreen(byte *p) { memcpy(p,I_VideoBuffer,SCREENWIDTH*SCREENHEIGHT); }
void I_SetPalette(byte *p)
{
    for (int i=0;i<256;i++) {
        int r=gammatable[usegamma][p[3*i]], g=gammatable[usegamma][p[3*i+1]], b=gammatable[usegamma][p[3*i+2]];
        rgb[3*i]=r; rgb[3*i+1]=g; rgb[3*i+2]=b;
        palette[i]=(r>>3)<<11|(g>>2)<<5|(b>>3);
    }
}
int I_GetPaletteIndex(int r,int g,int b)
{
    int best=0,diff=INT_MAX;
    for (int i=0;i<256;i++) {
        int dr=r-rgb[3*i],dg=g-rgb[3*i+1],db=b-rgb[3*i+2];
        int d=dr*dr+dg*dg+db*db;
        if (d<diff) { diff=d; best=i; }
    }
    return best;
}
void I_SetWindowTitle(char *s) { DG_SetWindowTitle(s); }
void I_UpdateNoBlit(void) {}
void I_BeginRead(void) {}
void I_EndRead(void) {}
void I_GraphicsCheckCommandLine(void) {}
void I_CheckIsScreensaver(void) {}
void I_SetGrabMouseCallback(grabmouse_callback_t f) { (void)f; }
void I_DisplayFPSDots(boolean on) { (void)on; }
void I_BindVideoVariables(void) {}
void I_EnableLoadingDisk(void) {}
void I_InitWindowTitle(void) {}
void I_InitWindowIcon(void) {}
