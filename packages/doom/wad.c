/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "port.h"
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
const char *doom_check_wad(const char *path, uint32_t *identity)
{
    FILE *f=fopen(path,"rb");
    if (!f) return "Could not open WAD";
    uint8_t h[12], d[16]; const char *error="Invalid IWAD header";
    if (fseek(f,0,SEEK_END)<0) goto done;
    long len=ftell(f);
    if (len<12 || len>32*1024*1024 || fseek(f,0,SEEK_SET)<0) goto done;
    if (fread(h,1,sizeof h,f)!=sizeof h || memcmp(h,"IWAD",4)) goto done;
    uint32_t count=le32(h+4),off=le32(h+8);
    if (!count || count>16384 || off<12 || off>(uint32_t)len || count>((uint32_t)len-off)/16) goto done;
    if (fseek(f,off,SEEK_SET)<0) goto done;
    uint32_t hash=2166136261u; int map=0;
    for (unsigned i=0;i<12;i++) hash=(hash^h[i])*16777619u;
    for (uint32_t i=0;i<count;i++) {
        error="Invalid WAD directory";
        if (fread(d,1,16,f)!=16) goto done;
        uint32_t pos=le32(d),size=le32(d+4);
        if (pos>(uint32_t)len || size>(uint32_t)len-pos || size>4*1024*1024) goto done;
        for (unsigned j=0;j<16;j++) hash=(hash^d[j])*16777619u;
        if (!memcmp(d+8,"E1M1\0\0\0\0",8) || !memcmp(d+8,"MAP01\0\0\0",8)) map=1;
    }
    if (!map) { error="No Doom maps in IWAD"; goto done; }
    *identity=hash; error=NULL;
done:
    fclose(f); return error;
}
