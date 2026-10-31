#include "../host/glaze_usb/pcconnect.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
static uint8_t received[8192], written[8192];
static unsigned count, position, written_len, chunk = 1;
static int file_peer, missing, read_error, close_error;
static unsigned remote_length, remote_offset, opens, reads, closes;
static uint8_t remote_data[9000];
volatile sig_atomic_t usb_cancelled;
static void file_reply(unsigned seq, unsigned op, unsigned result, unsigned error, unsigned payload)
{
    memset(received, 0, sizeof(received)); position=0;
    received[0]=0x11;received[1]=(uint8_t)seq;
    pc_put16(received+2,18+payload);pc_put16(received+4,op);
    pc_put32(received+10,result);pc_put32(received+14,error);count=18+payload;
}
static void file_request(void)
{
    unsigned op=pc_be16(written+4), seq=written[1];
    if(op==0x030c) {
        opens++;remote_offset=0;
        file_reply(seq,op,missing?~0u:17,missing?2:0,0);
    } else if(op==0x0319) {
        reads++;assert(pc_be32(written+10)==17);
        unsigned got=remote_length-remote_offset;if(got>4096)got=4096;
        file_reply(seq,op,!read_error,read_error?5:0,4+got);
        pc_put32(received+18,got);memcpy(received+22,remote_data+remote_offset,got);remote_offset+=got;
    } else if(op==0x0309) {
        closes++;file_reply(seq,op,!close_error,close_error?5:0,0);
    } else assert(!"comparison must not mutate CE files");
    pc_checksums(received,count);
}
int usb_write(struct usb_link *l, const void *p, size_t n, unsigned timeout)
{ (void)l; (void)timeout; assert(n <= sizeof(written)); memcpy(written, p, n); written_len = (unsigned)n; if(file_peer)file_request(); return 0; }
int usb_read(struct usb_link *l, void *p, size_t n, unsigned timeout)
{
    (void)l; (void)timeout;
    assert(position < count);
    if (n > count - position) n = count - position;
    if (n > chunk) n = chunk;
    memcpy(p, received + position, n); position += (unsigned)n; return (int)n;
}
int usb_lines(struct usb_link *l, int n) { (void)l; (void)n; return 0; }
int usb_open(struct usb_link *l, const char *path, int n) { (void)l; (void)path; (void)n; return 0; }
void usb_close(struct usb_link *l) { (void)l; }
static void reply(unsigned seq, unsigned op)
{
    memset(received, 0, sizeof(received)); position = 0;
    received[0] = 0x55; received[1] = 0x55; received[2] = 0x55;
    uint8_t *b = received + 3; b[0] = 0x11; b[1] = (uint8_t)seq;
    pc_put16(b + 2, 22); pc_put16(b + 4, op); pc_put32(b + 10, 1); pc_put32(b + 18, 0);
    pc_checksums(b, 22); count = 25;
}
int main(void)
{
    static const uint8_t query[] = {0x10,1,0,10,3,0x2d,0,0,0,0x4b};
    uint8_t q[10]; memcpy(q, query, 10); pc_checksums(q, 10);
    assert(!memcmp(q, query, 10) && pc_valid(q, 10));
    q[8] ^= 1; assert(!pc_valid(q, 10));
    static const uint8_t answer[] = {0x11,1,0,0x12,3,0x2d,0,0,0,0x54,0,0,0,0,0,0,0,0};
    assert(pc_valid(answer, sizeof(answer)));
    uint8_t path[100];
    assert(pc_path(path, sizeof(path), "A") == 6);
    assert(!memcmp(path, "\0\4\0A\0\0", 6));
    assert(pc_path(path, 5, "A") < 0 && pc_path(path, sizeof(path), "\xc3\xa9") < 0);
    for (chunk = 1; chunk < 32; chunk++) {
        struct pc_link p = {0}; uint8_t request[8];
        pc_put32(request, 17); pc_put32(request + 4, 4096);
        reply(1, 0x0319);
        assert(pc_call(&p, 0x0319, request, sizeof(request)) == 22);
        assert(p.result == 1 && p.error == 0 && written_len == 18 && pc_valid(written, written_len));
        assert(pc_be32(written + 10) == 17 && pc_be32(written + 14) == 4096);
    }
    struct pc_link p = {0}; reply(1, 0x0319); received[count - 1] ^= 1;
    assert(pc_call(&p, 0x0319, NULL, 0) == LIBUSB_ERROR_IO);
    /* Some ROM versions echo the request's checksum fields in replies. */
    memset(&p, 0, sizeof(p)); reply(1, 0x0319);
    uint8_t legacy_request[10] = {0x10, 1, 0, 10, 3, 0x19};
    pc_checksums(legacy_request, sizeof(legacy_request));
    memcpy(received + 3 + 6, legacy_request + 6, 4);
    assert(!pc_valid(received + 3, 22));
    assert(pc_call(&p, 0x0319, NULL, 0) == 22 && p.result == 1);
    unsigned old_position = position;
    assert(!pc_reboot(&p));
    assert(written_len == 10 && pc_valid(written, 10) && pc_be16(written + 4) == 0x032a);
    assert(position == old_position); /* SoftReset has no response to read. */
    char pathfile[]="/tmp/glaze-ce-match-XXXXXX";
    int fd=mkstemp(pathfile);assert(fd>=0);close(fd);
    for(unsigned i=0;i<sizeof(remote_data);i++)remote_data[i]=(uint8_t)(i*17);
    unsigned lengths[]={0,4096,5000,8192};
    file_peer=1;chunk=31;
    for(unsigned j=0;j<4;j++) {
        FILE *f=fopen(pathfile,"wb");assert(f);
        assert(fwrite(remote_data,1,lengths[j],f)==lengths[j]);fclose(f);
        remote_length=lengths[j];memset(&p,0,sizeof(p));
        unsigned before=closes;
        assert(pc_file_matches(&p,pathfile,"\\Nand Disk\\GlazeUSB\\kernel.bin")==1 && closes==before+1);
        remote_length++;memset(&p,0,sizeof(p));
        assert(pc_file_matches(&p,pathfile,"remote")==0);
        if(lengths[j]) {
            remote_length=lengths[j]-1;memset(&p,0,sizeof(p));assert(pc_file_matches(&p,pathfile,"remote")==0);
        }
    }
    remote_length=8192;remote_data[4099]^=1;memset(&p,0,sizeof(p));assert(pc_file_matches(&p,pathfile,"remote")==0);remote_data[4099]^=1;
    missing=1;memset(&p,0,sizeof(p));assert(pc_file_matches(&p,pathfile,"remote")==0);missing=0;
    read_error=1;memset(&p,0,sizeof(p));assert(pc_file_matches(&p,pathfile,"remote")==-1);read_error=0;
    close_error=1;memset(&p,0,sizeof(p));assert(pc_file_matches(&p,pathfile,"remote")==-1);close_error=0;
    unlink(pathfile);unsigned before=opens;
    assert(pc_file_matches(&p,pathfile,"remote")==-1 && opens==before);
    puts("usb-pcconnect: exact staged-file reuse, mismatches, EOF boundaries, read/close failures ALL PASS"); return 0;
}
