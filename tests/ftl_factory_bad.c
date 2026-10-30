#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ftl.h"
#define BLOCKS 64
static uint8_t flash[BLOCKS*32*512];
static unsigned bad, writes;
static int unavailable;
int flash_raw_is_bad(uint32_t b) { return unavailable ? -1 : (b < 32 && ((bad >> b)&1)); }
int flash_raw_read(uint32_t p, uint8_t *b) { assert(p < BLOCKS*32); memcpy(b,flash+p*512,512); return 0; }
int flash_raw_prog(uint32_t p, const uint8_t *b) {
    assert(!flash_raw_is_bad(p/32)); writes++;
    for(unsigned i=0;i<512;i++) flash[p*512+i]&=b[i]; return 0;
}
int flash_raw_erase(uint32_t b) {
    assert(!flash_raw_is_bad(b)); writes++; memset(flash+b*32*512,255,32*512); return 0;
}
void ftl_log(const char *m) { (void)m; }
int main(void) {
    static lfs_t fs;
    bad=1u|4u; memset(flash,255,sizeof(flash));
    assert(ftl_mount(&fs,BLOCKS)==1); assert(writes);
    struct ftl_stats s; ftl_get_stats(&s); assert(s.bad_blocks==2);
    assert(!lfs_unmount(&fs)); assert(!ftl_mount(&fs,BLOCKS)); assert(!lfs_unmount(&fs));
    bad=3; writes=0; memset(flash,255,sizeof(flash)); assert(ftl_mount(&fs,BLOCKS)<0 && !writes);
    unavailable=1; writes=0; assert(ftl_mount(&fs,BLOCKS)<0 && !writes);
    puts("ftl-factory: inventory before writes, bad BBT fallback, remount, both BBT bad and unavailable inventory PASS");
}
