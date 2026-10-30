/* Production NAND driver against the independent emulator controller model.
 * All storage is disposable host memory. No USB or physical NAND is opened. */
#include "hw/nand.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static nand_state_t model;
static uint32_t pending, latch;
static unsigned width;
static int status_override=-1, stuck;
static jmp_buf stopped;
static void flush(void)
{
    if (width) { nand_write(&model,pending,width,latch,0); width=0; }
}
static uint32_t *write32(uint32_t off)
{
    flush(); pending=off; width=4; latch=0; return &latch;
}
static uint8_t *write8(uint32_t off)
{
    flush(); pending=off; width=1; latch=0; return (uint8_t *)&latch;
}
static uint32_t read32(uint32_t off)
{
    flush();
    if (off==NAND_REG_XFER_STATUS && stuck) return 0;
    if (off==NAND_REG_XFER_ADDR && status_override>=0) return (uint32_t)status_override;
    return (uint32_t)nand_read(&model,off,4,0);
}
#define GLAZE_HW_H
#define NAND_PAGES_PER_BLOCK 32u
#define NAND_DMA_ENABLE (*write32(0x1cc0))
#define NAND_DMA_STATE (*write32(0x1cf4))
#define NAND_UNIT_CLOCK (*write32(0x1134))
#define NAND_XFER_EN0 (*write32(0xa400))
#define NAND_XFER_EN2 (*write32(0xa408))
#define NAND_XFER_PIO (*write32(0xa40c))
#define NAND_XFER_CTRL (*write32(NAND_REG_XFER_CTRL))
#define NAND_XFER_CMD (*write32(NAND_REG_XFER_CMD))
#define NAND_XFER_ADDR8 (*write8(NAND_REG_XFER_ADDR))
#define NAND_XFER_DATA32 read32(NAND_REG_XFER_ADDR)
#define NAND_XFER_ACK read32(NAND_REG_XFER_ACK)
#define NAND_XFER_STATUS read32(NAND_REG_XFER_STATUS)
#define NAND_XFER_KICK (*write32(NAND_REG_XFER_KICK))
#define NAND_XFER_MODE (*write32(NAND_REG_XFER_MODE))
#define NAND_STREAM8 (*write8(NAND_REG_STREAM_DATA))
#define NAND_STREAM32 read32(NAND_REG_STREAM_DATA)
#include "../kernel/drivers/nand.c"
#include "../kernel/nand_repair.c"
static struct gz_bootinfo boot={.flags=GZ_BOOT_NAND_REPAIR};
const struct gz_bootinfo *ram_boot=&boot;
static uint8_t userspace[4096];
int copy_from_as(struct addrspace *as,void *out,uint32_t addr,uint32_t n)
{ (void)as;assert(addr==0 && n<=sizeof(userspace));memcpy(out,userspace,n);return 0; }
static struct proc usb_process={.flags=PROC_USB};
static int repair_op(uint32_t op,const void *data,uint32_t n)
{
    if(n)memcpy(userspace,data,n);
    int r=sys_nand_repair(&usb_process,op,0,n);flush();return r;
}
static void stage(const uint8_t *data)
{
    uint8_t packet[4+7*528];
    for(unsigned page=0;page<32;) {
        unsigned n=32-page;if(n>7)n=7;
        gu_put32(packet,page);memcpy(packet+4,data+page*528,n*528);
        assert(!repair_op(GZ_NAND_REPAIR_DATA,packet,4+n*528));page+=n;
    }
}
static void test_repair(void)
{
    uint8_t bitmap[256]={0},header[12];
    static uint8_t before[32*528],after[32*528];
    memset(before,255,sizeof(before));memset(after,255,sizeof(after));
    for(unsigned p=0;p<32;p++)for(unsigned i=0;i<512;i++)after[p*528+i]=(uint8_t)(i+p);
    bitmap[20/8]|=1u<<(20%8); /* quarantined */
    bitmap[128+20/8]|=1u<<(20%8);
    assert(repair_op(GZ_NAND_REPAIR_BEGIN,bitmap,256)==GZ_EPERM);
    bitmap[128+20/8]=0;
    bitmap[128+21/8]|=1u<<(21%8);bitmap[128]|=1;
    struct proc other={.flags=PROC_FLASH};
    assert(sys_nand_repair(&other,GZ_NAND_REPAIR_AVAILABLE,0,0)==GZ_EPERM);
    boot.flags=0;assert(repair_op(GZ_NAND_REPAIR_BEGIN,bitmap,256)==GZ_EPERM);
    boot.flags=GZ_BOOT_NAND_REPAIR;
    assert(!repair_op(GZ_NAND_REPAIR_BEGIN,bitmap,256));
    gu_put32(header,0);gu_put32(header+4,~gu_crc(~0u,before,sizeof(before)));
    gu_put32(header+8,~gu_crc(~0u,after,sizeof(after)));
    assert(repair_op(GZ_NAND_REPAIR_BLOCK,header,12)==GZ_EPERM); /* block zero last */
    gu_put32(header,21);assert(!repair_op(GZ_NAND_REPAIR_BLOCK,header,12));
    assert(repair_op(GZ_NAND_REPAIR_COMMIT,NULL,0)==GZ_EINVAL);
    stage(before); /* wrong target CRC: refuse before erase */
    assert(repair_op(GZ_NAND_REPAIR_COMMIT,NULL,0)==GZ_ECORRUPT);
    assert(model.image[21*32*512]==255);
    assert(repair_op(GZ_NAND_REPAIR_BLOCK,header,12)==GZ_EPERM);
    assert(!repair_op(GZ_NAND_REPAIR_ABORT,NULL,0));
    assert(!repair_op(GZ_NAND_REPAIR_BEGIN,bitmap,256));
    assert(!repair_op(GZ_NAND_REPAIR_BLOCK,header,12));stage(after);
    assert(!repair_op(GZ_NAND_REPAIR_COMMIT,NULL,0));
    assert(!memcmp(model.image+21*32*512,after,512));
    assert(repair_op(GZ_NAND_REPAIR_END,NULL,0)==GZ_EINVAL);
    gu_put32(header,0);assert(!repair_op(GZ_NAND_REPAIR_BLOCK,header,12));stage(after);
    assert(!repair_op(GZ_NAND_REPAIR_COMMIT,NULL,0));
    assert(!repair_op(GZ_NAND_REPAIR_END,NULL,0));
    /* Source drift must be rejected, even with a valid staged target. */
    memset(bitmap+128,0,128);bitmap[128+21/8]|=1u<<(21%8);
    assert(!repair_op(GZ_NAND_REPAIR_BEGIN,bitmap,256));
    gu_put32(header,21);assert(!repair_op(GZ_NAND_REPAIR_BLOCK,header,12));stage(after);
    assert(repair_op(GZ_NAND_REPAIR_COMMIT,NULL,0)==GZ_ECORRUPT);
    assert(!repair_op(GZ_NAND_REPAIR_ABORT,NULL,0));
}
void panic(const char *format, ...) { (void)format; longjmp(stopped,1); }
void host_io_emit_serial(int ch) { (void)ch; }
bool host_io_stdout_enabled(void) { return false; }
int main(void)
{
    uint8_t *image=malloc(NAND_IMAGE_SIZE),*oob=malloc(NAND_OOB_IMAGE_SIZE);
    assert(image && oob); memset(image,255,NAND_IMAGE_SIZE);memset(oob,255,NAND_OOB_IMAGE_SIZE);
    nand_init(&model,image,NAND_IMAGE_SIZE,NAND_IMAGE_SIZE); model.oob=oob;
    nand_reset(); flush(); assert(nand_status()==0xc0);
    uint8_t raw[528] __attribute__((aligned(4))),readback[528] __attribute__((aligned(4)));
    for (unsigned i=0;i<sizeof(raw);i++) raw[i]=(uint8_t)(i*17+i/7);
    unsigned page=17*32+3;
    assert(!nand_program_raw(page,raw));flush();
    nand_read_raw(page,readback);flush();assert(!memcmp(raw,readback,528));
    uint8_t ones[528];memset(ones,255,sizeof(ones));
    assert(!nand_program_raw(page,ones));flush();
    nand_read_raw(page,readback);flush();assert(!memcmp(raw,readback,528));
    assert(!nand_erase_block(17));flush();
    nand_read_raw(page,readback);flush();assert(!memcmp(ones,readback,528));
    assert(!nand_program_page(page,raw));flush();
    nand_read_raw(page,readback);flush();
    assert(!memcmp(raw,readback,512) && !memcmp(ones,readback+512,16));
    oob[20*32*16+5]=0; /* Factory-bad block: model must reject and preserve it. */
    assert(nand_erase_block(20)==-1);flush();assert(oob[20*32*16+5]==0);
    assert(nand_program_raw(20*32+3,raw)==-1);flush();assert(image[(20*32+3)*512]==255);
    assert(nand_status()==0xc1);
    assert(nand_program_raw(32768,raw)==-1 && nand_erase_block(1024)==-1);
    status_override=0x40; assert(nand_erase_block(17)==-1);flush();
    status_override=0x80; assert(nand_program_page(page,raw)==-1);flush();
    status_override=-1;stuck=1;
    if (!setjmp(stopped)) { nand_program_raw(page,raw);assert(!"timeout returned"); }
    flush(); assert(model.xfer_regs[(NAND_REG_XFER_CTRL-NAND_XFER_BASE)/4]==0);
    stuck=0;test_repair();
    free(image);free(oob);
    puts("nand-program: raw data/OOB, one-way programming, erase, failure/status/protection, bounds and timeout PASS");
}
