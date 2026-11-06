#include "../../apps/eyefi/protocol.h"
#include <glaze/abi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct mock { unsigned writes, polls, sleeps, query; uint32_t seq; int fail, stale, bad, remove; };
static int rd(void *ctx, unsigned file, uint8_t *b)
{
    struct mock *m = ctx;
    if (m->fail) return m->fail;
    memset(b, 0, EYEFI_SIZE);
    if (file == EYEFI_RSPC) {
        uint32_t seq = m->seq;
        if (m->writes && (++m->polls == 1 || m->stale)) --seq;
        for (unsigned i=0;i<4;++i) b[i]=(uint8_t)(seq>>(8*i));
    } else {
        assert(file == EYEFI_RSPM);
        b[0] = m->bad ? 255 : m->query == 1 ? 6 : m->query == 2 ? 4 : m->query == 13 ? 3 : 1;
        memcpy(b+1, "testxx", b[0] > 6 ? 6 : b[0]);
    }
    return 0;
}
static int wr(void *ctx, unsigned file, const uint8_t *b)
{
    struct mock *m=ctx;
    if (m->fail) return m->fail;
    if (file == EYEFI_REQM) {
        assert(!m->writes && b[0]=='o'); m->query=b[1];
        for (unsigned i=2;i<EYEFI_SIZE;++i) assert(!b[i]);
    } else {
        assert(file == EYEFI_REQC && m->writes==1);
        m->seq=(uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;
        for (unsigned i=4;i<EYEFI_SIZE;++i) assert(!b[i]);
    }
    ++m->writes;
    return 0;
}
static void nap(void *ctx, unsigned ms)
{
    struct mock *m=ctx; assert(ms==300); ++m->sleeps;
    if (m->remove) m->fail=GZ_EIO;
}
int main(void)
{
    uint8_t b[EYEFI_SIZE];
    struct mock m={0};
    struct eyefi_io io={&m,rd,wr,nap};
    unsigned queries[]={1,2,10,11,13};
    for (unsigned i=0;i<5;++i) {
        m=(struct mock){.seq=0x12ffffff};
        assert(eyefi_query(&io,queries[i],b)>0);
        assert(m.writes==2 && m.sleeps==1 && m.seq==0x13000000);
    }
    m=(struct mock){.seq=0xffffffff};
    assert(eyefi_query(&io,1,b)==6 && m.seq==1);
    m=(struct mock){.stale=1};
    assert(eyefi_query(&io,1,b)==GZ_ETIMEDOUT && m.sleeps==50);
    m=(struct mock){.bad=1};
    assert(eyefi_query(&io,1,b)==GZ_ECORRUPT);
    m=(struct mock){.bad=1};
    assert(eyefi_query(&io,2,b)==GZ_ECORRUPT);
    m=(struct mock){.fail=GZ_ENOENT};
    assert(eyefi_query(&io,1,b)==GZ_ENOENT && !m.writes);
    m=(struct mock){.remove=1};
    assert(eyefi_query(&io,1,b)==GZ_EIO);
    m=(struct mock){0};
    assert(eyefi_query(&io,3,b)==GZ_EINVAL && !m.writes);
    assert(eyefi_query(&io,0xfd,b)==GZ_EINVAL && !m.writes);
    puts("eyefi: protocol PASS");
}
