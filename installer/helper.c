#include "client.h"
#include "network.h"
#include "compat.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
/* This executable deliberately has no network, CF, upload or shell interface. */
void debug_host_close(struct usb_link *l) { (void)l; }
int debug_host_read(struct usb_link *l,void *p,size_t n,unsigned t) { (void)l;(void)p;(void)n;(void)t;return -1; }
int debug_host_write(struct usb_link *l,const void *p,size_t n,unsigned t) { (void)l;(void)p;(void)n;(void)t;return -1; }
int main(int argc,char **argv) {
    installer_binary(); setbuf(stdout,NULL); setbuf(stderr,NULL);
    if(argc<3) return 2;
    struct usb_link l={0}; struct gu_client c; int r=-1;
    const char *device=strcmp(argv[1],"auto") ? argv[1] : NULL;
    /* Reset before claiming an interface: stale CE configuration can make
     * claiming/set-configuration fail after Glaze takes over the controller. */
    if(!strcmp(argv[2],"reset-link") && argc==3) {
        r=usb_reset_link(device,0);
        printf("{\"event\":\"complete\",\"ok\":%s}\n",r ? "false" : "true");
        return r ? 1 : 0;
    }
    if(usb_open(&l,device,0)) return 1;
    if(!strcmp(argv[2],"probe") && argc==3) {
        printf("{\"event\":\"device\",\"location\":\"%u",l.bus);
        for(unsigned j=0;j<l.port_count;j++) printf(".%u",l.ports[j]);
        printf("\",\"runtime\":\"%s\"}\n",l.revision==0x0100 ? "Glaze" : "CE");
        r=0;
        if(l.revision==0x0100) {
            r=gu_hello(&c,&l);
            if(!r) printf("{\"event\":\"capabilities\",\"value\":%u}\n",c.caps);
        }
    } else if(!strcmp(argv[2],"boot") && argc==4) {
        r=l.revision==0x0100 ? gu_hello(&c,&l) : 0;
        if(!r) r=l.revision==0x0100 ? gu_reload(&c,argv[3]) : ce_boot(&l,argv[3],-1);
    } else if(l.revision==0x0100 && !gu_hello(&c,&l)) {
        if(!strcmp(argv[2],"backup") && argc==4) r=gu_nand_backup(&c,argv[3]);
        else if(!strcmp(argv[2],"apply") && argc==6) r=gu_nand_apply(&c,argv[3],argv[4],argv[5],false);
        else if(!strcmp(argv[2],"cold-reboot") && argc==3 && (c.caps&GU_CAP_REBOOT_CE)) r=gu_call(&c,GU_REBOOT_CE,NULL,0);
    }
    usb_close(&l);
    printf("{\"event\":\"complete\",\"ok\":%s}\n",r ? "false" : "true");
    return r ? 1 : 0;
}
