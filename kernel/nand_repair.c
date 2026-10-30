/* One complete physical block is staged and checked before any erase.
 * This path is absent from normal read-only/RW/NAND boot permissions. */
#include "kernel.h"
#include "proc.h"
#include "drivers/nand.h"
#include <glaze/bootinfo.h>
#include <glaze/nand_repair.h>
#include <glaze/usb_wire.h>
static struct {
    bool active,failed,staging;
    uint8_t bad[128],remaining[128];
    uint32_t blocks,block,pages,before,after;
    uint8_t data[GZ_NAND_REPAIR_BLOCK_BYTES] __attribute__((aligned(4)));
} repair;
static uint8_t request[GU_PAYLOAD] __attribute__((aligned(4)));
static uint8_t verify[528] __attribute__((aligned(4)));
static int fail(int error) { repair.failed=true; return error; }
static int commit(void)
{
    if (!repair.staging || repair.pages!=32) return GZ_EINVAL;
    if (~gu_crc(~0u,repair.data,sizeof(repair.data))!=repair.after) return fail(GZ_ECORRUPT);
    uint32_t crc=~0u;
    for (unsigned i=0;i<32;i++) {
        nand_read_raw(repair.block*32+i,verify);crc=gu_crc(crc,verify,sizeof(verify));
    }
    if (~crc!=repair.before) return fail(GZ_ECORRUPT);
    if (nand_erase_block(repair.block)) return fail(GZ_EIO);
    for (unsigned i=0;i<32;i++) {
        nand_read_raw(repair.block*32+i,verify);
        for (unsigned j=0;j<528;j++) if (verify[j]!=255) return fail(GZ_EIO);
    }
    for (unsigned i=0;i<32;i++) {
        uint8_t *page=repair.data+i*528;
        bool erased=true;
        for (unsigned j=0;j<528;j++) erased=erased && page[j]==255;
        if (!erased && nand_program_raw(repair.block*32+i,page)) return fail(GZ_EIO);
        nand_read_raw(repair.block*32+i,verify);
        if (memcmp(page,verify,528)) return fail(GZ_EIO);
    }
    repair.remaining[repair.block/8]&=~(1u<<(repair.block%8));
    repair.blocks--;repair.staging=false;
    memset(repair.data,0,sizeof(repair.data));
    return 0;
}
int sys_nand_repair(struct proc *p,uint32_t op,uint32_t buf,uint32_t len)
{
    if (!(p->flags&PROC_USB) || !ram_boot || (ram_boot->flags&~GZ_BOOT_FROM_ROM)!=GZ_BOOT_NAND_REPAIR) return GZ_EPERM;
    if (op==GZ_NAND_REPAIR_AVAILABLE) return len ? GZ_EINVAL : 0;
    if (op==GZ_NAND_REPAIR_ABORT) {
        if(len)return GZ_EINVAL;
        memset(&repair,0,sizeof(repair));return 0;
    }
    if (len>sizeof(request) || (len && copy_from_as(&p->as,request,buf,len))) return GZ_EINVAL;
    if (op==GZ_NAND_REPAIR_BEGIN) {
        if (len!=256 || repair.active) return GZ_EINVAL;
        memset(&repair,0,sizeof(repair));
        memcpy(repair.bad,request,128);memcpy(repair.remaining,request+128,128);
        for (unsigned b=0;b<1024;b++) {
            bool changed=(repair.remaining[b/8]>>(b%8))&1;
            if (changed && ((repair.bad[b/8]>>(b%8))&1)) return GZ_EPERM;
            repair.blocks+=changed;
        }
        if (!repair.blocks) return GZ_EINVAL;
        repair.active=true;return 0;
    }
    if (!repair.active || repair.failed) return GZ_EPERM;
    if (op==GZ_NAND_REPAIR_BLOCK) {
        if (len!=12 || repair.staging) return GZ_EINVAL;
        uint32_t block=gu_get32(request);
        if (block>=1024 || !((repair.remaining[block/8]>>(block%8))&1) || (!block && repair.blocks>1)) return GZ_EPERM;
        repair.block=block;repair.before=gu_get32(request+4);repair.after=gu_get32(request+8);
        repair.pages=0;repair.staging=true;return 0;
    }
    if (op==GZ_NAND_REPAIR_DATA) {
        if (!repair.staging || len<532 || (len-4)%528 || gu_get32(request)!=repair.pages ||
            (len-4)/528>32-repair.pages) return GZ_EINVAL;
        memcpy(repair.data+repair.pages*528,request+4,len-4);repair.pages+=(len-4)/528;
        return 0;
    }
    if (op==GZ_NAND_REPAIR_COMMIT) return len ? GZ_EINVAL : commit();
    if (op==GZ_NAND_REPAIR_END) {
        if (len || repair.blocks || repair.staging) return GZ_EINVAL;
        memset(&repair,0,sizeof(repair));return 0;
    }
    return GZ_EINVAL;
}
