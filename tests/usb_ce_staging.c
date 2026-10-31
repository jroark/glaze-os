/* Run the production CE staging planner against filesystem/space fixtures. */
#include "../host/glaze_usb/client.c"
#include <assert.h>
static uint64_t available;
static unsigned matching, checks, mkdirs, volume_checks;
static int inspect_failure, allow_boot;
static unsigned launches;
volatile sig_atomic_t usb_cancelled;
static const char *names[]={"UsbProbe.exe","GlazeBoot.exe","kernel.bin","romfs.img","glaze.cfg"};
int pc_connect(struct pc_link *p,struct usb_link *u){(void)p;(void)u;assert(allow_boot);return 0;}
int pc_file_matches(struct pc_link *p,const char *local,const char *remote)
{
    (void)p;(void)local;checks++;
    const char *name=strrchr(remote,'\\');assert(name);name++;
    for(unsigned i=0;i<5;i++)if(!strcmp(name,names[i]))return inspect_failure==(int)i+1?-1:!!(matching&(1u<<i));
    assert(0);return -1;
}
int pc_free_space(struct pc_link *p,const char *path,uint64_t *space)
{(void)p;assert(!strcmp(path,"\\Nand Disk"));volume_checks++;*space=available;return 0;}
int pc_mkdir(struct pc_link *p,const char *path)
{(void)p;assert(!strcmp(path,"\\Nand Disk\\GlazeUSB"));mkdirs++;return 0;}
int pc_put(struct pc_link *p,const char *a,const char *b){(void)p;(void)a;(void)b;assert(0);return -1;}
int pc_get(struct pc_link *p,const char *a,const char *b){(void)p;(void)a;(void)b;assert(0);return -1;}
int pc_run(struct pc_link *p,const char *a,const char *b){(void)p;assert(allow_boot && strstr(a,"GlazeBoot.exe") && !strcmp(b,"--usb-boot"));launches++;return 0;}
int usb_write(struct usb_link *u,const void *b,size_t n,unsigned t){(void)u;(void)b;(void)n;(void)t;assert(0);return -1;}
int usb_read(struct usb_link *u,void *b,size_t n,unsigned t){(void)u;(void)b;(void)n;(void)t;assert(0);return -1;}
int usb_lines(struct usb_link *u,int n){(void)u;(void)n;assert(0);return -1;}
static void reset(uint64_t free_bytes,unsigned matches)
{available=free_bytes;matching=matches;checks=mkdirs=volume_checks=0;inspect_failure=0;}
int main(void)
{
    char dir[]="/tmp/glaze-ce-staging-XXXXXX",local[1024],folder[512];assert(mkdtemp(dir));
    const unsigned sizes[]={2048,8192,131072,4*1024*1024,32};
    struct ce_artifact files[5];
    for(unsigned i=0;i<5;i++) {
        snprintf(local,sizeof(local),"%s/%s",dir,names[i]);FILE *f=fopen(local,"wb");assert(f);
        assert(!fseek(f,sizes[i]-1,SEEK_SET));assert(fputc(0,f)!=EOF);assert(!fclose(f));
        files[i]=(struct ce_artifact){.name=names[i]};
    }
    struct pc_link p={0};
    reset(65536,31);assert(!ce_folder(&p,dir,files,5,folder,-1));
    assert(checks==5 && mkdirs==1 && volume_checks==1);
    for(unsigned i=0;i<5;i++)assert(files[i].reuse);
    reset(65536,15);assert(ce_folder(&p,dir,files,5,folder,-1)<0 && !mkdirs);
    reset(65536+4096,15);assert(!ce_folder(&p,dir,files,5,folder,-1));assert(!files[4].reuse);
    reset(1024*1024,31&~8u);assert(ce_folder(&p,dir,files,5,folder,-1)<0 && !mkdirs);
    reset(6*1024*1024,0);assert(!ce_folder(&p,dir,files,5,folder,-1));
    for(unsigned i=0;i<5;i++)assert(!files[i].reuse);
    reset(6*1024*1024,31);inspect_failure=4;
    assert(ce_folder(&p,dir,files,5,folder,-1)<0 && !mkdirs && checks==4);
    /* Missing local inputs must fail before querying or modifying CE. */
    snprintf(local,sizeof(local),"%s/glaze.cfg",dir);assert(!unlink(local));reset(6*1024*1024,0);
    assert(ce_folder(&p,dir,files,5,folder,-1)<0 && !volume_checks && !mkdirs);
    /* Skipping the optional CE probe needs no UsbProbe.exe, and launches
     * only GlazeBoot. Kernel/payload validation remains required. */
    FILE *f=fopen(local,"wb");assert(f);fputs("nand=ro\n",f);fclose(f);
    snprintf(local,sizeof(local),"%s/kernel.bin",dir);f=fopen(local,"r+b");assert(f);
    uint8_t header[32]={0};gu_put32(header,0x424b5a47);gu_put32(header+4,0x80010000);
    gu_put32(header+8,0x80010000);gu_put32(header+12,sizes[2]-4096);gu_put32(header+16,0x80010000+sizes[2]-4096);
    assert(fwrite(header,1,sizeof(header),f)==sizeof(header));fclose(f);
    snprintf(local,sizeof(local),"%s/romfs.img",dir);f=fopen(local,"r+b");assert(f);assert(fwrite("GZRF",1,4,f)==4);fclose(f);
    snprintf(local,sizeof(local),"%s/UsbProbe.exe",dir);assert(!unlink(local));
    allow_boot=1;reset(65536,31);struct usb_link u={0};
    assert(!ce_boot_options(&u,dir,-1,0) && checks==4 && launches==1);
    assert(ce_boot(&u,dir,-1)<0 && launches==1); /* default still verifies CE */
    snprintf(local,sizeof(local),"%s/kernel.bin",dir);f=fopen(local,"r+b");assert(f);fputc(0,f);fclose(f);
    assert(ce_boot_options(&u,dir,-1,0)<0 && launches==1);
    snprintf(local,sizeof(local),"%s/glaze.cfg",dir);assert(!unlink(local));
    for(unsigned i=1;i<4;i++){snprintf(local,sizeof(local),"%s/%s",dir,names[i]);assert(!unlink(local));}
    assert(!rmdir(dir));
    puts("usb-ce-staging: reuse on low space, changed-file allocation, source validation and fail-closed inspection PASS");
    return 0;
}
