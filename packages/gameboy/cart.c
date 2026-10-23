#include "gameboy.h"
#include <string.h>
uint32_t gb_hash(const void *data,size_t size)
{
    const uint8_t *p=data;
    uint32_t hash=2166136261u;
    while (size--) hash=(hash^*p++)*16777619u;
    return hash;
}
const char *gb_validate(const uint8_t *p,size_t size,struct gb_cart *out)
{
    struct gb_cart c={0};
    if (!p || size<0x150) return "Cartridge header is incomplete";
    if (size>GB_ROM_MAX || p[0x148]>8) return "Unsupported ROM size (maximum 8 MiB)";
    c.rom_size=32768u<<p[0x148];
    if (size!=c.rom_size) return "ROM length does not match its header";
    unsigned type=p[0x147];
    bool has_ram=false;
    switch(type) {
    case 0: break;
    case 8: case 9: has_ram=true; c.battery=type==9; break;
    case 1: case 2: case 3: c.mapper=1; has_ram=type!=1; c.battery=type==3; break;
    case 5: case 6: c.mapper=2; c.ram_size=512; c.battery=type==6; break;
    case 15: case 16: c.clock=true; c.battery=true; has_ram=type==16; c.mapper=3; break;
    case 17: case 18: case 19: c.mapper=3; has_ram=type!=17; c.battery=type==19; break;
    case 25: case 26: case 27: c.mapper=5; has_ram=type!=25; c.battery=type==27; break;
    case 28: case 29: case 30: c.mapper=15; has_ram=type!=28; c.battery=type==30; break;
    default:return "Unsupported cartridge controller";
    }
    static const uint32_t sizes[]={0,2048,8192,32768,131072,65536};
    if (p[0x149]>=sizeof sizes/sizeof sizes[0]) return "Unsupported cartridge RAM size";
    uint32_t declared=sizes[p[0x149]];
    if (has_ram) {
        if (!declared) return "Cartridge RAM is missing from header";
        c.ram_size=declared;
    } else if (declared) return "RAM declaration conflicts with cartridge type";
    if ((!c.mapper && (c.rom_size!=32768 || c.ram_size>8192)) ||
        (c.mapper==1 && (c.rom_size>2097152 || c.ram_size>32768)) ||
        (c.mapper==2 && c.rom_size>262144) ||
        (c.mapper==3 && (c.rom_size>2097152 || c.ram_size>32768)) ||
        (c.mapper==15 && c.ram_size>65536)) return "Cartridge exceeds controller limits";
    if (p[0x143]!=0 && p[0x143]!=0x80 && p[0x143]!=0xc0 && (p[0x143]&0x80))
        return "Unsupported Game Boy hardware flag";
    c.cgb=(p[0x143]&0x80)!=0;
    unsigned n=c.cgb ? 11 : 16;
    for (unsigned i=0;i<n;i++) {
        uint8_t v=p[0x134+i];
        c.title[i]=v>=32 && v<127 ? (char)v : ' ';
    }
    uint8_t sum=0;
    for(unsigned i=0x134;i<=0x14c;i++) sum=sum-p[i]-1;
    c.checksum_warning=sum!=p[0x14d];
    c.hash=gb_hash(p,size);
    *out=c;
    return NULL;
}
