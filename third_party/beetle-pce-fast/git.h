#ifndef GLAZE_MDFN_GIT_H
#define GLAZE_MDFN_GIT_H
#include "mednafen-types.h"
typedef struct { int x,y,w,h; } MDFN_Rect;
typedef struct { uint16 *pixels; unsigned pitch; } MDFN_Surface;
typedef struct { MDFN_Surface *surface; MDFN_Rect DisplayRect; bool skip; } EmulateSpecStruct;
#endif
