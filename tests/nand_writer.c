/* Host transaction -> wire peer -> production kernel repair state machine.
 * Faults operate only on a disposable RAM NAND. Low-level controller I/O is
 * tested independently by nand_program.c against the emulator's model. */
#include "../host/glaze_usb/client.h"
#include "../kernel/kernel.h"
#include "../kernel/proc.h"
#include <glaze/bootinfo.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define SIZE (1024u*32u*528u)
static uint8_t device[SIZE],userspace[4096];
static unsigned erases,programs,order[1024],completed;
static int erase_failure,readback_failure,final_failure;
static struct gz_bootinfo boot={.flags=GZ_BOOT_NAND_REPAIR};
const struct gz_bootinfo *ram_boot=&boot;
int copy_from_as(struct addrspace *as,void *out,uint32_t addr,uint32_t n)
{ (void)as;assert(addr==0 && n<=sizeof(userspace));memcpy(out,userspace,n);return 0; }
void nand_read_raw(uint32_t page,uint8_t out[528])
{ memcpy(out,device+page*528,528);if(readback_failure && programs)out[0]^=1; }
int nand_program_raw(uint32_t page,const uint8_t in[528])
{ for(unsigned i=0;i<528;i++)device[page*528+i]&=in[i];programs++;return 0; }
int nand_erase_block(uint32_t block)
{ erases++;if(erase_failure)return -1;memset(device+block*32*528,255,32*528);return 0; }
#include "../kernel/nand_repair.c"
int gu_call(struct gu_client *c,uint32_t op,const void *data,uint32_t n)
{
    const uint8_t *b=data;
    if(op==GU_NAND_READ) {
        assert(n==8);unsigned page=gu_get32(b),count=gu_get32(b+4);
        assert(count && count<=7 && page+count<=32768);
        memcpy(c->frame+GU_HEADER,device+page*528,count*528);
        if(final_failure && completed==2)c->frame[GU_HEADER]^=1;
        return (int)(count*528);
    }
    assert(op==GU_NAND_REPAIR && n>=4);
    memcpy(userspace,b+4,n-4);
    struct proc p={.flags=PROC_USB};
    unsigned command=gu_get32(b);
    int r=sys_nand_repair(&p,command,0,n-4);
    if(!r && command==GZ_NAND_REPAIR_BLOCK) order[completed]=gu_get32(b+4);
    if(!r && command==GZ_NAND_REPAIR_COMMIT)completed++;
    return r;
}
static void write_file(const char *path,const void *data,size_t n)
{ FILE *f=fopen(path,"wb");assert(f && fwrite(data,1,n,f)==n && !fclose(f)); }
static void digest(const void *data,size_t n,uint8_t out[32])
{ wc_Sha256 h;assert(!wc_InitSha256(&h));assert(!wc_Sha256Update(&h,data,(word32)n));assert(!wc_Sha256Final(&h,out));wc_Sha256Free(&h); }
int main(void)
{
    char dir[]="/tmp/glaze-write-test-XXXXXX",source_path[256],target_path[256],plan_path[256];
    assert(mkdtemp(dir));snprintf(source_path,sizeof(source_path),"%s/source",dir);
    snprintf(target_path,sizeof(target_path),"%s/target",dir);snprintf(plan_path,sizeof(plan_path),"%s/plan",dir);
    uint8_t *source=malloc(SIZE),*target=malloc(SIZE);assert(source && target);
    memset(source,255,SIZE);source[20*32*528+517]=0;memcpy(target,source,SIZE);
    memset(target+30*32*528,0x5a,32*528);memset(target,0xa5,32*528);
    uint8_t plan[204]={0};memcpy(plan,"GZNDTX1\0",8);
    digest(source,SIZE,plan+8);digest(target,SIZE,plan+40);plan[72+20/8]|=1u<<(20%8);
    gu_put32(plan+200,~gu_crc(~0u,plan,200));
    write_file(source_path,source,SIZE);write_file(target_path,target,SIZE);write_file(plan_path,plan,sizeof(plan));
    struct gu_client c={.caps=GU_CAP_NAND_REPAIR|GU_CAP_NAND_BACKUP};
    memcpy(device,source,SIZE);
    assert(!gu_nand_apply(&c,source_path,target_path,plan_path));
    assert(completed==2 && order[0]==30 && order[1]==0 && !memcmp(device,target,SIZE));
    assert(!memcmp(device+20*32*528,source+20*32*528,32*528));
    /* Same verified transaction path restores every original data/OOB byte. */
    digest(target,SIZE,plan+8);digest(source,SIZE,plan+40);
    gu_put32(plan+200,~gu_crc(~0u,plan,200));write_file(plan_path,plan,sizeof(plan));completed=0;
    assert(!gu_nand_apply(&c,target_path,source_path,plan_path) && !memcmp(device,source,SIZE));
    digest(source,SIZE,plan+8);digest(target,SIZE,plan+40);
    gu_put32(plan+200,~gu_crc(~0u,plan,200));write_file(plan_path,plan,sizeof(plan));
    memcpy(device,source,SIZE);device[33]^=1;erases=programs=completed=0;
    assert(gu_nand_apply(&c,source_path,target_path,plan_path)<0 && !erases && !programs);
    memcpy(device,source,SIZE);erase_failure=1;
    assert(gu_nand_apply(&c,source_path,target_path,plan_path)<0 && !completed && !programs);
    assert(!memcmp(device,source,32*528)); /* boot block not committed */
    erase_failure=0;erases=0;readback_failure=1;
    assert(gu_nand_apply(&c,source_path,target_path,plan_path)<0 && !completed && programs==1);
    assert(!memcmp(device,source,32*528));
    readback_failure=0;memcpy(device,source,SIZE);programs=completed=0;final_failure=1;
    assert(gu_nand_apply(&c,source_path,target_path,plan_path)<0 && completed==2);
    final_failure=0;memcpy(device,source,SIZE);programs=completed=erases=0;
    plan[8]^=1;gu_put32(plan+200,~gu_crc(~0u,plan,200));write_file(plan_path,plan,sizeof(plan));
    assert(gu_nand_apply(&c,source_path,target_path,plan_path)<0 && !erases && !programs);
    free(source);free(target);unlink(source_path);unlink(target_path);unlink(plan_path);rmdir(dir);
    puts("nand-writer: full preflight, image binding, quarantine preservation, boot block last, erase/readback/final failure stops PASS");
}
