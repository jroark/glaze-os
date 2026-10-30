/* Explicit NAND transaction, only exposed by a separate nand=repair RAM boot.
 * Files and the entire physical source are verified before the first erase. */
#include "client.h"
#include <glaze/nand_repair.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define RAW_BYTES (1024u*GZ_NAND_REPAIR_BLOCK_BYTES)
static uint8_t *load(const char *path,size_t size)
{
    FILE *f=fopen(path,"rb");if(!f)return NULL;
    struct stat st;
    if(fstat(fileno(f),&st) || !S_ISREG(st.st_mode) || st.st_size!=(off_t)size){fclose(f);return NULL;}
    uint8_t *b=malloc(size);
    if(!b || fread(b,1,size,f)!=size){free(b);b=NULL;}
    fclose(f);return b;
}
static int hash(const uint8_t *data,size_t size,const uint8_t *expected)
{
    wc_Sha256 s={0};uint8_t digest[32];
    int r=wc_InitSha256(&s);
    if(!r)r=wc_Sha256Update(&s,data,(word32)size);
    if(!r)r=wc_Sha256Final(&s,digest);
    wc_Sha256Free(&s);
    return r || memcmp(digest,expected,32) ? -1 : 0;
}
static int rpc(struct gu_client *c,uint32_t op,const void *data,uint32_t n)
{
    uint8_t b[GU_PAYLOAD];if(n>sizeof(b)-4)return -1;
    gu_put32(b,op);if(n)memcpy(b+4,data,n);
    return gu_call(c,GU_NAND_REPAIR,b,n+4);
}
static int compare(struct gu_client *c,const uint8_t *expected,const char *phase)
{
    uint8_t request[8];
    for(uint32_t page=0;page<32768;) {
        unsigned n=32768-page;if(n>7)n=7;
        gu_put32(request,page);gu_put32(request+4,n);
        if(gu_call(c,GU_NAND_READ,request,sizeof(request))!=(int)(n*528) ||
           memcmp(c->frame+GU_HEADER,expected+page*528,n*528)) {
            fprintf(stderr,"NAND %s differs at page %u; stopped\n",phase,page);return -1;
        }
        page+=n;
        if(page/2048!=(page-n)/2048)fprintf(stderr,"NAND %s: %u/32768 pages\n",phase,page);
    }
    return 0;
}
int gu_nand_apply(struct gu_client *c,const char *source_path,const char *target_path,const char *plan_path)
{
    if(!(c->caps&GU_CAP_NAND_REPAIR)) {
        fputs("Requires an explicit nand=repair RAM boot; ordinary read-only boots cannot program NAND.\n",stderr);return -1;
    }
    uint8_t *source=load(source_path,RAW_BYTES),*target=load(target_path,RAW_BYTES),*plan=load(plan_path,204);
    int result=-1;unsigned changed=0;
    uint8_t masks[256]={0};
    if(!source || !target || !plan || memcmp(plan,"GZNDTX1\0",8) ||
       ~gu_crc(~0u,plan,200)!=gu_get32(plan+200) || hash(source,RAW_BYTES,plan+8) || hash(target,RAW_BYTES,plan+40)) goto done;
    memcpy(masks,plan+72,128);
    for(unsigned b=0;b<1024;b++) {
        if(!memcmp(source+b*GZ_NAND_REPAIR_BLOCK_BYTES,target+b*GZ_NAND_REPAIR_BLOCK_BYTES,GZ_NAND_REPAIR_BLOCK_BYTES))continue;
        if((masks[b/8]>>(b%8))&1){fputs("Plan changes a quarantined block; refused\n",stderr);goto done;}
        masks[128+b/8]|=1u<<(b%8);changed++;
    }
    if(!changed){fputs("Source and target are identical; checking NAND without programming\n",stderr);result=compare(c,target,"verification");goto done;}
    if(compare(c,source,"preflight"))goto done;
    if(rpc(c,GZ_NAND_REPAIR_BEGIN,masks,sizeof(masks)))goto done;
    for(unsigned index=0;index<1024;index++) {
        unsigned block=1023-index;
        if(!((masks[128+block/8]>>(block%8))&1))continue;
        uint8_t header[12],packet[4+7*528];
        const uint8_t *before=source+block*GZ_NAND_REPAIR_BLOCK_BYTES,*after=target+block*GZ_NAND_REPAIR_BLOCK_BYTES;
        gu_put32(header,block);gu_put32(header+4,~gu_crc(~0u,before,GZ_NAND_REPAIR_BLOCK_BYTES));
        gu_put32(header+8,~gu_crc(~0u,after,GZ_NAND_REPAIR_BLOCK_BYTES));
        if(rpc(c,GZ_NAND_REPAIR_BLOCK,header,sizeof(header)))goto failed;
        for(unsigned page=0;page<32;) {
            unsigned n=32-page;if(n>7)n=7;
            gu_put32(packet,page);memcpy(packet+4,after+page*528,n*528);
            if(rpc(c,GZ_NAND_REPAIR_DATA,packet,4+n*528))goto failed;
            page+=n;
        }
        if(rpc(c,GZ_NAND_REPAIR_COMMIT,NULL,0))goto failed;
        fprintf(stderr,"NAND verified block %u (%u remaining)\n",block,--changed);
    }
    if(compare(c,target,"final verification") || rpc(c,GZ_NAND_REPAIR_END,NULL,0))goto failed;
    fputs("Entire physical NAND matches the target, including spare data; no reboot requested.\n",stderr);
    result=0;goto done;
failed:
    rpc(c,GZ_NAND_REPAIR_ABORT,NULL,0);
    fputs("NAND transaction stopped. Keep the recovery CF inserted; use a fresh raw snapshot to plan restoration.\n",stderr);
done:
    free(source);free(target);free(plan);return result;
}
