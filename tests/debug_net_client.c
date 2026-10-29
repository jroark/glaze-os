/* Exercise TLS record fragmentation/coalescing using the production host I/O. */
#include "../host/glaze_usb/network.h"
#include <glaze/usb_wire.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
volatile sig_atomic_t usb_cancelled;
int main(int argc,char **argv)
{
    assert(argc==2);
    struct usb_link link={0};assert(!debug_host_open(&link,"127.0.0.1",argv[1]));
    uint8_t packet[GU_FRAME*3],data[GU_PAYLOAD],input[213];
    for(unsigned i=0;i<sizeof data;i++)data[i]=(uint8_t)(i*17);
    size_t used=0;
    for(unsigned id=1;id<=3;id++)used+=gu_encode(packet+used,99,id,0,data,sizeof data-id);
    /* Frames span many TLS records and each record is split across reads. */
    for(size_t i=0;i<used;) { size_t n=used-i<137?used-i:137;assert(!debug_host_write(&link,packet+i,n,5000));i+=n; }
    struct gu_parser parser={0};unsigned count=0;
    while(count<3) {
        int n=debug_host_read(&link,input,sizeof input,5000);assert(n>0);
        for(int i=0;i<n;i++)if(gu_feed(&parser,input[i])==1) {
            count++;const uint8_t *f=parser.bytes;
            assert(gu_get32(f+8)==count && gu_get32(f+12)==sizeof data-count);
            assert(!memcmp(f+GU_HEADER,data,sizeof data-count));
        }
    }
    debug_host_close(&link);puts("debug TLS fragmented/coalesced frames: PASS");return 0;
}
