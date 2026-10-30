#include "../kernel/kernel.h"
#include "../kernel/proc.h"
#include <glaze/bootinfo.h>
#include <glaze/nand_image.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct gz_bootinfo boot={.flags=0,.ramfs_blocks=128};
const struct gz_bootinfo *ram_boot=&boot;
static struct gz_nand_manifest physical;
static int physical_mode, reads, programs, erases, write_fail;
static uint8_t copied[528];
const struct gz_nand_manifest *boot_nand_manifest(void) { return physical_mode ? &physical : NULL; }
void boot_nand_read(uint32_t p, void *b) { (void)p; (void)b; }
int copy_to_as(struct addrspace *as,uint32_t dest,const void *buf,uint32_t n) { (void)as;(void)dest; assert(n<=sizeof(copied));memcpy(copied,buf,n);return 0; }
int copy_from_as(struct addrspace *as,void *buf,uint32_t src,uint32_t n) { (void)as;(void)src;memset(buf,0,n);return 0; }
void nand_read_raw(uint32_t p,uint8_t b[528]) { reads++; for(unsigned i=0;i<528;i++)b[i]=(uint8_t)(p+i); }
void nand_read_page(uint32_t p,void *b) { (void)p;(void)b;reads++; }
int nand_program_page(uint32_t p,const void *b) { (void)p;(void)b;programs++;return write_fail; }
int nand_erase_block(uint32_t b) { (void)b;erases++;return write_fail; }
void nand_reset(void) {}
uint8_t nand_status(void) { return 0xc0; }
uint64_t time_us(void) { return 0; }
void kprintf(const char *s,...) { (void)s; }
#include "../kernel/drivers/flash.c"
int main(void) {
    struct proc usb={.flags=PROC_USB}, fs={.flags=PROC_FLASH}, other={0};
    assert(sys_nand_raw_read(&other,0,1)==GZ_EPERM);
    assert(sys_nand_chip_status(&other)==GZ_EPERM && sys_nand_chip_status(&usb)==0xc0);
    assert(!sys_nand_raw_read(&usb,32767,1) && reads==1);
    for(unsigned i=0;i<528;i++) assert(copied[i]==(uint8_t)(32767+i));
    assert(sys_nand_raw_read(&usb,32768,1)==GZ_EINVAL);
    boot.flags=GZ_BOOT_NAND_OFF; assert(sys_nand_raw_read(&usb,0,1)==GZ_EPERM);
    boot.flags=GZ_BOOT_NAND_RW; assert(sys_nand_raw_read(&usb,0,1)==GZ_EPERM);
    ram_boot=NULL; assert(sys_nand_raw_read(&usb,0,1)==GZ_EPERM);
    physical_mode=1; physical.fs_blocks=684;
    physical.bad[350/8] |= 1u<<(350%8);
    assert(sys_flash_bad(&fs,30)==1 && sys_flash_bad(&fs,29)==0);
    assert(sys_flash_read(&fs,30*32,1)==GZ_EIO);
    assert(sys_flash_prog(&fs,30*32,1)==GZ_EIO);
    assert(sys_flash_erase(&fs,30)==GZ_EIO && !programs && !erases && reads==1);
    assert(sys_flash_erase(&fs,684)==GZ_EINVAL);
    assert(sys_flash_bad(&other,30)==GZ_EPERM);
    assert(!sys_flash_info(&fs,1));
    struct gz_flash_info info; memcpy(&info,copied,sizeof(info));
    assert(info.blocks==684 && (info.flags&GZ_FLASH_BADMAP));
    write_fail=1;
    assert(sys_flash_prog(&fs,29*32,1)==GZ_EIO && programs==1);
    assert(sys_flash_erase(&fs,29)==GZ_EIO && erases==1);
    ram_boot=&boot;boot.flags=GZ_BOOT_NAND_REPAIR;
    assert(!sys_flash_info(&fs,1));memcpy(&info,copied,sizeof(info));
    assert((info.flags&GZ_FLASH_RAM) && !(info.flags&GZ_FLASH_NAND));
    assert(sys_nand_read(&fs,0,1)==GZ_ENOENT && sys_nand_prog(&fs,320*32,1)==GZ_ENOENT);
    puts("nand-raw-policy: privileges, read-only handoff, physical bounds, unchanged bytes, bad-block and reserve protection PASS");
}
