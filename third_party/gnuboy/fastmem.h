#ifndef GLAZE_GB_FASTMEM_H
#define GLAZE_GB_FASTMEM_H
#include "mem.h"
static inline byte readb(int address) {
    unsigned a=(unsigned)address&0xffff;
    byte *p=mbc.rmap[a>>12];
    return p ? p[a&4095] : mem_read(a);
}
static inline void writeb(int address, byte value) {
    unsigned a=(unsigned)address&0xffff;
    byte *p=mbc.wmap[a>>12];
    if (p) p[a&4095]=value; else mem_write(a,value);
}
static inline int readw(int a) {
    int lo=readb(a); return lo|((int)readb(a+1)<<8);
}
static inline void writew(int a,int w) { writeb(a,w); writeb(a+1,w>>8); }
static inline byte readhi(int a) { return readb(a|0xff00); }
static inline void writehi(int a,byte b) { writeb(a|0xff00,b); }
#endif
