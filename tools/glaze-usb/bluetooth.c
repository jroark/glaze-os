/* Diagnostic serial client; PINs and link keys are never written to files. */
#include "client.h"
#include <glaze/bluetooth.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/random.h>
#include <time.h>
static uint64_t millis(void)
{struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000+(unsigned)ts.tv_nsec/1000000;}
static int status(struct gu_client *c,uint32_t *id,unsigned *state,unsigned *available)
{
    uint8_t p[4];gu_put32(p,BT_SERIAL_STATUS);
    if(gu_call(c,GU_BT_SERIAL,p,4)!=40)return -1;
    const uint8_t *b=c->frame+GU_HEADER;
    *state=gu_get32(b);*id=gu_get32(b+4);*available=gu_get32(b+20);
    return *state>BT_SERIAL_FAILED?-1:0;
}
int gu_bt_serial(struct gu_client *c,const char *cmd,int argc,char **argv)
{
    if(!(c->caps&GU_CAP_BT_SERIAL)){fputs("Reload a build with Bluetooth serial support.\n",stderr);return -1;}
    uint8_t request[208]={0};uint32_t id=0;unsigned state,available;
    if(!strcmp(cmd,"bt-connect")) {
        unsigned address[6];char tail;
        if(strlen(argv[0])!=17 || sscanf(argv[0],"%2x:%2x:%2x:%2x:%2x:%2x%c",address,address+1,address+2,address+3,address+4,address+5,&tail)!=6)return -1;
        gu_put32(request,BT_SERIAL_CONNECT);
        for(unsigned i=0;i<6;i++)request[4+i]=(uint8_t)address[5-i];
        char generated[7];const char *pin;
        if(argc>=2)pin=argv[1];
        else {uint32_t random;if(getentropy(&random,sizeof(random)))return -1;snprintf(generated,sizeof(generated),"%06u",random%1000000);pin=generated;}
        size_t n=strlen(pin);if(!n || n>16)return -1;
        request[10]=(uint8_t)n;memcpy(request+11,pin,n);
        printf("Pairing PIN: %s — enter it on the Mac if prompted.\n",pin);fflush(stdout);
        unsigned channel=0;
        if(argc==3){char *end;unsigned long value=strtoul(argv[2],&end,10);if(!*argv[2] || *end || value<1 || value>30)return -1;channel=(unsigned)value;}
        request[11+n]=(uint8_t)channel;
        int r=gu_call(c,GU_BT_SERIAL,request,(uint32_t)(12+n));memset(request,0,sizeof(request));memset(generated,0,sizeof(generated));
        if(r)return -1;
        static const char *names[]={"Idle","Connecting","Pairing","Encrypting","Discovering services","Opening RFCOMM","Connected","Closing","Failed"};
        uint64_t end=millis()+120000;unsigned previous=~0u;
        while(!usb_cancelled && millis()<end){
            if(status(c,&id,&state,&available))return -1;
            const uint8_t *p=c->frame+GU_HEADER;
            if(state!=previous){printf("Serial: %s error=%d channel=%u hci=%02x authenticated=%u encrypted=%u\n",names[state],(int32_t)gu_get32(p+8),p[38],p[39],gu_get32(p+24),gu_get32(p+28));fflush(stdout);previous=state;}
            if(state==BT_SERIAL_CONNECTED)return 0;
            if(state==BT_SERIAL_IDLE || state==BT_SERIAL_FAILED)return -1;
            usleep(250000);
        }
        gu_put32(request,BT_SERIAL_CLOSE);gu_put32(request+4,id);gu_call(c,GU_BT_SERIAL,request,8);return -1;
    }
    if(status(c,&id,&state,&available))return -1;
    if(!strcmp(cmd,"bt-serial-status")){
        const uint8_t *p=c->frame+GU_HEADER;
        printf("Serial: state=%u id=%u error=%d sent=%u received=%u available=%u authenticated=%u encrypted=%u channel=%u\n",state,id,(int32_t)gu_get32(p+8),gu_get32(p+12),gu_get32(p+16),available,gu_get32(p+24),gu_get32(p+28),p[38]);return 0;
    }
    if(!strcmp(cmd,"bt-disconnect")){if(state==BT_SERIAL_IDLE)return 0;gu_put32(request,BT_SERIAL_CLOSE);gu_put32(request+4,id);return gu_call(c,GU_BT_SERIAL,request,8);}
    if(state!=BT_SERIAL_CONNECTED){fputs("No connected serial service.\n",stderr);return -1;}
    if(available){fputs("Unread serial data exists; refusing to discard it for an echo test.\n",stderr);return -1;}
    unsigned length=(unsigned)strlen(argv[0]);if(!length || length>192)return -1;
    unsigned sent=0,received=0;uint8_t echo[192];uint64_t end=millis()+10000;
    while((sent<length || received<length) && !usb_cancelled && millis()<end){
        if(sent<length){gu_put32(request,BT_SERIAL_WRITE);gu_put32(request+4,id);memcpy(request+8,argv[0]+sent,length-sent);
            int n=gu_call(c,GU_BT_SERIAL,request,8+length-sent);if(n!=4)return -1;
            unsigned count=gu_get32(c->frame+GU_HEADER);if(count>length-sent)return -1;sent+=count;}
        gu_put32(request,BT_SERIAL_READ);gu_put32(request+4,id);gu_put32(request+8,length-received);
        int n=gu_call(c,GU_BT_SERIAL,request,12);if(n<0 || (unsigned)n>length-received)return -1;
        memcpy(echo+received,c->frame+GU_HEADER,(unsigned)n);received+=(unsigned)n;
        if(received<length)usleep(20000);
    }
    if(received!=length || memcmp(echo,argv[0],length)){fputs("Serial echo mismatch or timeout.\n",stderr);return -1;}
    printf("Serial echo verified: %u bytes\n",length);return 0;
}

static int audio_status(struct gu_client *c)
{
    uint8_t request[4];gu_put32(request,BT_AUDIO_STATUS);
    if(gu_call(c,GU_BT_AUDIO,request,4)!=56)return -1;
    const uint8_t *p=c->frame+GU_HEADER;
    printf("Audio: state=%u id=%u error=%d rate=%u channels=%u bitpool=%u packets=%u frames=%u queued=%u underruns=%u authenticated=%u encrypted=%u hci=%02x\n",
           gu_get32(p),gu_get32(p+4),(int32_t)gu_get32(p+8),gu_get32(p+12),gu_get32(p+16),gu_get32(p+20),gu_get32(p+24),gu_get32(p+28),gu_get32(p+32),gu_get32(p+36),gu_get32(p+40),gu_get32(p+44),p[54]);
    fflush(stdout);return (int)gu_get32(p);
}
int gu_bt_audio(struct gu_client *c,const char *cmd,int argc,char **argv)
{
    if(!(c->caps&GU_CAP_BT_AUDIO)){fputs("Reload a build with Bluetooth audio support.\n",stderr);return -1;}
    uint8_t request[32]={0};
    if(!strcmp(cmd,"bt-audio-status"))return audio_status(c)<0?-1:0;
    if(!strcmp(cmd,"bt-audio-disconnect")){gu_put32(request,BT_AUDIO_CLOSE);return gu_call(c,GU_BT_AUDIO,request,4);}
    if(!strcmp(cmd,"bt-audio-tone")){
        unsigned ms=1000;if(argc){char *end;unsigned long n=strtoul(argv[0],&end,10);if(!*argv[0] || *end || n>3000)return -1;ms=(unsigned)n;}
        gu_put32(request,BT_AUDIO_TONE);gu_put32(request+4,ms);return gu_call(c,GU_BT_AUDIO,request,8);
    }
    if(strcmp(cmd,"bt-audio-connect"))return -1;
    unsigned address[6];char tail;
    if(strlen(argv[0])!=17 || sscanf(argv[0],"%2x:%2x:%2x:%2x:%2x:%2x%c",address,address+1,address+2,address+3,address+4,address+5,&tail)!=6)return -1;
    const char *pin=argc>1?argv[1]:"0000";size_t n=strlen(pin);if(!n || n>16)return -1;
    gu_put32(request,BT_AUDIO_CONNECT);for(unsigned i=0;i<6;i++)request[4+i]=(uint8_t)address[5-i];request[10]=(uint8_t)n;memcpy(request+11,pin,n);
    int error=gu_call(c,GU_BT_AUDIO,request,11+(unsigned)n);memset(request,0,sizeof(request));if(error)return -1;
    uint64_t end=millis()+100000;int previous=-1;
    while(!usb_cancelled && millis()<end){
        uint8_t req[4];gu_put32(req,BT_AUDIO_STATUS);if(gu_call(c,GU_BT_AUDIO,req,4)!=56)return -1;
        const uint8_t *p=c->frame+GU_HEADER;int state=(int)gu_get32(p);
        if(state!=previous){state=audio_status(c);previous=state;}
        if(state==BT_AUDIO_STREAMING)return 0;
        if(state<=BT_AUDIO_IDLE || state>=BT_AUDIO_FAILED)return -1;
        usleep(250000);
    }
    gu_put32(request,BT_AUDIO_CLOSE);gu_call(c,GU_BT_AUDIO,request,4);return -1;
}
