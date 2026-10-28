#include "../host/glaze_usb/pcconnect.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned files, deletes, removes;
static int folder, unknown, directory_file, fail_delete;
static const char *const names[] = {"UsbProbe.exe","usb-probe.txt","GlazeBoot.exe","kernel.bin","romfs.img","glaze.cfg","glazeboot.log"};
int pc_path(uint8_t *out, unsigned cap, const char *path)
{ assert(strlen(path)+1<=cap); strcpy((char *)out,path); return (int)strlen(path)+1; }
int pc_call(struct pc_link *p,unsigned op,const void *data,unsigned n)
{
    (void)n; const char *path=data, *root="\\Nand Disk\\GlazeUSB";
    assert(!strncmp(path,root,strlen(root)));
    int index=-1;
    if (strcmp(path,root)) {
        assert(path[strlen(root)]=='\\');
        for(unsigned i=0;i<7;i++)if(!strcmp(path+strlen(root)+1,names[i]))index=(int)i;
        assert(index>=0); /* Unknown names must never be inspected/deleted. */
    }
    p->result=0;p->error=0;
    if(op==0x0313) {
        int exists=folder && (index<0 || (files & (1u<<index)));
        p->result=exists?(index<0 || directory_file==index+1?0x10:0x20):~0u;p->error=exists?0:2;
    } else if(op==0x030e) {
        assert(index>=0 && folder && (files&(1u<<index)));deletes++;
        if(fail_delete) {p->error=5;return 18;}
        files&=~(1u<<index);p->result=1;
    } else if(op==0x031a) {
        assert(index<0 && !files);removes++;
        if(unknown)p->error=145;else{folder=0;p->result=1;}
    } else assert(0);
    return 18;
}
static void reset(void)
{files=127;folder=1;unknown=directory_file=fail_delete=0;deletes=removes=0;}
int main(void)
{
    struct pc_link p={0};reset();assert(!pc_clean_nand(&p,1));assert(files==127 && !deletes && !removes);
    reset();assert(!pc_clean_nand(&p,0));assert(!folder && !files && deletes==7 && removes==1);
    assert(!pc_clean_nand(&p,0) && deletes==7);
    reset();unknown=1;assert(!pc_clean_nand(&p,0));assert(folder && !files && unknown);
    reset();directory_file=5;assert(pc_clean_nand(&p,0)<0);assert(files==127 && !deletes && !removes);
    reset();fail_delete=1;assert(pc_clean_nand(&p,0)<0);assert(files==127 && folder && !removes);
    puts("usb-cleanup: ALL PASS");return 0;
}
