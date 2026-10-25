#include "radio_host.h"
#include "../servers/audio/radio.h"
#include "../servers/audio/radio_mp3.h"
#include "../apps/browser/plat.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct { pthread_mutex_t lock; pthread_cond_t cond; int count; } sem = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0
};
void gz_mutex_lock(struct gz_mutex *m) { assert(!pthread_mutex_lock(&m->native)); }
void gz_mutex_unlock(struct gz_mutex *m) { assert(!pthread_mutex_unlock(&m->native)); }
int gz_sem_create(int count) { sem.count=count; return 1; }
int gz_sem_destroy(int id) { (void)id; return 0; }
int gz_sem_wait(int id, int timeout)
{
    (void)id; (void)timeout;
    pthread_mutex_lock(&sem.lock);
    while (!sem.count) pthread_cond_wait(&sem.cond, &sem.lock);
    sem.count--;
    pthread_mutex_unlock(&sem.lock); return 0;
}
int gz_sem_post(int id)
{
    (void)id;
    pthread_mutex_lock(&sem.lock); sem.count++; pthread_cond_signal(&sem.cond);
    pthread_mutex_unlock(&sem.lock); return 0;
}
struct start { void (*fn)(void *); void *arg; };
static void *start(void *arg)
{
    struct start s=*(struct start *)arg; free(arg); s.fn(s.arg); return NULL;
}
int gz_thread_create(void (*fn)(void *), void *arg)
{
    struct start *s=malloc(sizeof(*s)); assert(s); *s=(struct start){fn,arg};
    pthread_t thread; assert(!pthread_create(&thread,NULL,start,s)); pthread_detach(thread); return 1;
}
uint32_t gz_time_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint32_t)(t.tv_sec*1000+t.tv_nsec/1000000);
}
void gz_sleep_ms(uint32_t ms) { usleep(ms*1000); }
int net_resolve(const char *host, uint32_t *addr, int timeout)
{
    (void)timeout;
    struct addrinfo hints={0}, *res;
    hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    if (getaddrinfo(host,NULL,&hints,&res)) return GZ_ENOENT;
    *addr=((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res); return 0;
}
long long plat_time(void) { return time(NULL); }
void plat_noise(uint8_t *out,int n)
{
    FILE *f=fopen("/dev/urandom","rb"); assert(f); assert(fread(out,1,(size_t)n,f)==(size_t)n); fclose(f);
}
void plat_load_seed(uint8_t *s) { (void)s; }
void plat_save_seed(const uint8_t *s) { (void)s; }
const void *plat_roots(uint32_t *n)
{
    static uint8_t roots[8192]; *n=0;
    const char *path=getenv("RADIO_TEST_ROOTS");
    if (path) { FILE *f=fopen(path,"rb"); assert(f); *n=(uint32_t)fread(roots,1,sizeof(roots),f); fclose(f); }
    return roots;
}
static void play(const char *url,const char *id)
{
    struct radio_request req={0};
    snprintf(req.id,sizeof(req.id),"%s",id); strcpy(req.name,"Fixture Station");
    snprintf(req.playlist,sizeof(req.playlist),"%s",url);
    assert(!radio_start(&req));
}
int main(int argc,char **argv)
{
    assert(argc >= 3);
    play(argv[2],"first");
    if (!strcmp(argv[1],"switch")) {
        assert(argc==4); gz_sleep_ms(100); play(argv[3],"second");
    } else if (!strcmp(argv[1],"full")) {
        uint32_t now=gz_time_ms();
        uint32_t epoch;
        while (radio_buffered(&epoch) < RADIO_CAPACITY && gz_time_ms()-now<5000) gz_sleep_ms(10);
        assert(radio_buffered(&epoch)==RADIO_CAPACITY);
        assert(argc==4); play(argv[3],"second");
    }
    struct radio_mp3 decoder={0};
    uint32_t started=gz_time_ms(), epoch=0;
    int frames=0; struct radio_state st={0};
    while (gz_time_ms()-started < 4000) {
        radio_snapshot(&st);
        assert(st.buffered_bytes <= RADIO_CAPACITY);
        if (!strcmp(argv[1],"error") && st.phase==RADIO_ERROR) break;
        uint32_t next;
        radio_buffered(&next);
        if (next != epoch) { assert(!radio_mp3_reset(&decoder)); epoch=next; }
        uint8_t data[4096];
        int n=radio_read(epoch,data,MIN((int)sizeof(data),(int)sizeof(decoder.input)-decoder.len));
        radio_mp3_feed(&decoder,data,n);
        MP3FrameInfo info; int16_t pcm[2304];
        int count=radio_mp3_frame(&decoder,pcm,&info);
        if (count) { frames+=count; radio_output(true,(uint32_t)info.bitrate/1000); }
        gz_sleep_ms(1);
    }
    radio_snapshot(&st);
    if (!strcmp(argv[1],"error")) {
        assert(argc==4 && st.phase==RADIO_ERROR && st.error==(uint32_t)atoi(argv[3]));
    } else {
        assert(frames>1000);
        assert(!strcmp(st.title,"Fixture Artist - Song"));
        if (argc==4) assert(!strcmp(st.id,"second"));
    }
    radio_stop();
    radio_snapshot(&st); assert(st.phase==RADIO_STOPPED && st.buffered_bytes==0);
    gz_sleep_ms(300);
    radio_snapshot(&st); assert(st.phase==RADIO_STOPPED && st.buffered_bytes==0 && !st.title[0]);
    radio_mp3_close(&decoder);
    printf("radio worker: %s PASS (%d decoded frames)\n",argv[1],frames);
    return 0;
}
