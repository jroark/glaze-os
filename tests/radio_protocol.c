/* Fragment every boundary of real protocol sequences; no network required. */
#include "../servers/audio/radio_protocol.h"
#include "../servers/audio/radio_mp3.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct capture { char data[8192], title[192]; int len, cancel; };
static int sink(void *arg, const uint8_t *p, int n)
{
    struct capture *c = arg;
    if (c->cancel) return -200;
    assert(n <= (int)sizeof(c->data)-c->len);
    memcpy(c->data+c->len, p, (size_t)n); c->len += n;
    return 0;
}
static void title(void *arg, const char *s) { snprintf(((struct capture *)arg)->title,192,"%s",s); }
static int feed(struct radio_http *h, const char *s, int n, int fragment)
{
    int rv = 0;
    for (int off = 0; off < n && !rv;) {
        int take = n-off < fragment ? n-off : fragment;
        rv = radio_http_feed(h, (const uint8_t *)s+off, take); off += take;
    }
    return rv;
}
static void protocols(void)
{
    for (int fragment = 1; fragment < 130; fragment++) {
        struct radio_http h;
        struct capture c = {0};
        radio_http_init(&h, sink, title, &c);
        const char *plain = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
        assert(feed(&h,plain,(int)strlen(plain),fragment) == 1);
        assert(c.len == 5 && !memcmp(c.data,"hello",5));
        assert(radio_http_eof(&h) == 1);
        memset(&c,0,sizeof(c)); radio_http_init(&h,sink,title,&c);
        const char *chunks = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2;ext=yes\r\nhe\r\n3\r\nllo\r\n0\r\nX-Trailer: yes\r\n\r\n";
        assert(feed(&h,chunks,(int)strlen(chunks),fragment) == 1);
        assert(c.len == 5 && !memcmp(c.data,"hello",5));
        memset(&c,0,sizeof(c)); radio_http_init(&h,sink,title,&c);
        const char *redirect = "HTTP/1.1 302 Found\r\nLocation: /new.pls\r\n\r\n";
        assert(feed(&h,redirect,(int)strlen(redirect),fragment) == 1);
        assert(h.status == 302 && !strcmp(h.location,"/new.pls") && c.len == 0);
        memset(&c,0,sizeof(c)); radio_http_init(&h,sink,title,&c);
        char icy[256] = "ICY 200 OK\r\nicy-metaint: 3\r\nContent-Type: audio/mpeg\r\n\r\nabc";
        int n = (int)strlen(icy);
        icy[n++] = 2;
        memcpy(icy+n,"StreamTitle='Artist - Song';",28); n += 28;
        memset(icy+n,0,4); n+=4;
        memcpy(icy+n,"def\0ghi",7); n+=7;
        assert(feed(&h,icy,n,fragment) == 0);
        assert(c.len == 9 && !memcmp(c.data,"abcdefghi",9));
        assert(!strcmp(c.title,"Artist - Song"));
        assert(radio_http_eof(&h) == 1);
    }
    const char *bad[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 2147483648\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n",
        "HTTP/1.1 200 OK\r\nicy-metaint: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n80000000\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx!",
        "garbage\r\n\r\n"
    };
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        struct radio_http h; struct capture c = {0};
        radio_http_init(&h,sink,title,&c);
        assert(feed(&h,bad[i],(int)strlen(bad[i]),1) == -1);
    }
    struct radio_http h; struct capture c = {0};
    radio_http_init(&h,sink,title,&c);
    const char *short_body="HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nhello";
    assert(!feed(&h,short_body,(int)strlen(short_body),1));
    assert(radio_http_eof(&h) == -1);
    radio_http_init(&h,sink,title,&c); c.cancel=1;
    const char *cancel="HTTP/1.1 200 OK\r\n\r\nx";
    assert(feed(&h,cancel,(int)strlen(cancel),1) == -200);
    char large[9000]; memset(large,'x',sizeof(large));
    radio_http_init(&h,sink,title,&c);
    assert(feed(&h,large,sizeof(large),1) == -1);
    struct radio_playlist p;
    char pls[]="[playlist]\r\nFile1=https://one/stream\r\nTitle1=Station\r\nFile2=http://two:80/stream\r\nFile3=ftp://bad\r\nFile4=https://one/stream\r\n";
    assert(!radio_parse_playlist(pls,&p) && p.count == 2);
    assert(!strcmp(p.urls[1],"http://two:80/stream"));
    char invalid[]="<html>not a playlist</html>";
    assert(radio_parse_playlist(invalid,&p) < 0);
    struct radio_mp3 d = {0};
    assert(!radio_mp3_reset(&d));
    uint8_t edge[8192]; memset(edge, 0, sizeof(edge));
    edge[8190] = 0xff; edge[8191] = 0xfb;
    assert(radio_mp3_feed(&d, edge, sizeof(edge)) == (int)sizeof(edge));
    MP3FrameInfo info; int16_t pcm[2304];
    assert(radio_mp3_frame(&d, pcm, &info) == 0 && d.len == 2);
    radio_mp3_close(&d);
    puts("radio protocol: fragmented HTTP/ICY, redirects, malformed input, cancellation, playlists PASS");
}
static uint64_t decode(const uint8_t *data, int len, int fragment, int *frames)
{
    struct radio_mp3 d = {0};
    assert(!radio_mp3_reset(&d));
    uint64_t hash = 1469598103934665603ull;
    int pos=0;
    *frames=0;
    for (;;) {
        int take = len-pos < fragment ? len-pos : fragment;
        int used = radio_mp3_feed(&d,data+pos,take);
        pos += used;
        int16_t pcm[2304]; MP3FrameInfo info;
        int n = radio_mp3_frame(&d,pcm,&info);
        if (n) {
            assert(info.nChans == 1 || info.nChans == 2);
            *frames += n;
            for (int i=0;i<info.outputSamps;i++) { hash ^= (uint16_t)pcm[i]; hash *= 1099511628211ull; }
        } else if (pos == len) break;
        else assert(used > 0);
    }
    radio_mp3_close(&d);
    return hash;
}
int main(int argc,char **argv)
{
    protocols();
    assert(argc==2);
    FILE *f=fopen(argv[1],"rb"); assert(f);
    fseek(f,0,SEEK_END); int len=(int)ftell(f); rewind(f);
    uint8_t *data=malloc((size_t)len); assert(data);
    assert(fread(data,1,(size_t)len,f)==(size_t)len); fclose(f);
    int expected_frames;
    uint64_t expected=decode(data,len,4096,&expected_frames);
    assert(expected_frames > 1000);
    int fragments[] = {1,2,3,7,31,127,509,1024,8192};
    for (size_t i=0;i<sizeof(fragments)/sizeof(fragments[0]);i++) {
        int frames;
        uint64_t hash=decode(data,len,fragments[i],&frames);
        assert(frames==expected_frames && hash==expected);
    }
    free(data);
    printf("radio MP3: identical PCM for all 10 fragmentation sizes (%d frames) PASS\n",expected_frames);
    return 0;
}
