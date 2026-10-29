/* Bidirectional nearest-neighbour point clouds. Uniform scale/translation
 * normalization and arc-length sampling remove position, size, pen speed,
 * stroke direction and stroke order; orientation is deliberately retained.
 * No floating point, libm, heap allocation or network/model dependency. */
#include <glaze/handwriting.h>
#include <string.h>
#include "ink_shapes.h"
#define CLOUD 32u
struct point {int16_t x,y;};
static struct point templates[ARRAY_SIZE(ink_shapes)][CLOUD];
static bool prepared;
static bool coordinate(int x,int y){return x>=0 && x<=1023 && y>=0 && y<=1023;}
void ui_ink_clear(struct ui_ink *ink){memset(ink,0,sizeof(*ink));}
static bool append(struct ui_ink *ink,int x,int y)
{
    if(!coordinate(x,y) || !ink->drawing)return false;
    if(ink->count>UI_INK_POINTS){ink->overflow=true;return false;}
    if(ink->count && ink->count>ink->start){
        const struct ui_ink_point *last=&ink->points[ink->count-1];
        if(last->x==x && last->y==y)return false;
    }
    if(ink->count>=UI_INK_POINTS){ink->overflow=true;return false;}
    ink->points[ink->count++]=(struct ui_ink_point){x,y,(uint16_t)(ink->strokes-1)};return true;
}
bool ui_ink_begin(struct ui_ink *ink,int x,int y)
{
    if(!coordinate(x,y) || ink->drawing || ink->overflow)return false;
    if(ink->strokes>=UI_INK_STROKES){ink->overflow=true;return false;}
    ink->start=ink->count;ink->strokes++;ink->drawing=true;return append(ink,x,y);
}
bool ui_ink_move(struct ui_ink *ink,int x,int y){return append(ink,x,y);}
void ui_ink_end(struct ui_ink *ink,int x,int y){if(ink->drawing)append(ink,x,y);ink->drawing=false;}
void ui_ink_cancel(struct ui_ink *ink)
{
    if(!ink->drawing)return;
    ink->count=ink->start;if(ink->strokes)ink->strokes--;ink->drawing=false;
}
static unsigned root(unsigned value)
{
    unsigned result=0,bit=1u<<30;
    while(bit>value)bit>>=2;
    while(bit){if(value>=result+bit){value-=result+bit;result=(result>>1)+bit;}else result>>=1;bit>>=2;}
    return result;
}
static unsigned length(struct point a,struct point b)
{int x=a.x-b.x,y=a.y-b.y;return root((unsigned)(x*x+y*y));}
static bool cloud(const struct ui_ink *ink,struct point out[CLOUD])
{
    if(!ink->count || ink->count>UI_INK_POINTS || !ink->strokes || ink->strokes>UI_INK_STROKES || ink->overflow || ink->drawing)return false;
    int minx=1023,miny=1023,maxx=0,maxy=0;
    unsigned starts[UI_INK_STROKES]={0},ends[UI_INK_STROKES]={0};
    for(unsigned i=0;i<ink->count;i++){
        const struct ui_ink_point *p=&ink->points[i];
        if(!coordinate(p->x,p->y) || p->stroke>=ink->strokes || (!i && p->stroke) ||
            (i && p->stroke!=ink->points[i-1].stroke && p->stroke!=ink->points[i-1].stroke+1))return false;
        if(!i || p->stroke!=ink->points[i-1].stroke)starts[p->stroke]=i;
        ends[p->stroke]=i;
        minx=MIN(minx,p->x);maxx=MAX(maxx,p->x);miny=MIN(miny,p->y);maxy=MAX(maxy,p->y);
    }
    if(ink->points[ink->count-1].stroke+1!=ink->strokes)return false;
    int scale=MAX(maxx-minx,maxy-miny);if(scale<8)return false;
    struct point normalized[UI_INK_POINTS];
    int ox=(256-(maxx-minx)*256/scale)/2,oy=(256-(maxy-miny)*256/scale)/2;
    for(unsigned i=0;i<ink->count;i++)normalized[i]=(struct point){(ink->points[i].x-minx)*256/scale+ox,(ink->points[i].y-miny)*256/scale+oy};
    unsigned lengths[UI_INK_STROKES]={0},samples[UI_INK_STROKES],remainders[UI_INK_STROKES];
    unsigned total=0;
    for(unsigned s=0;s<ink->strokes;s++){
        for(unsigned i=starts[s]+1;i<=ends[s];i++)lengths[s]+=length(normalized[i-1],normalized[i]);
        total+=lengths[s];
    }
    if(total<16)return false;
    unsigned remaining=CLOUD-ink->strokes,allocated=ink->strokes;
    for(unsigned s=0;s<ink->strokes;s++){
        samples[s]=1+remaining*lengths[s]/total;remainders[s]=remaining*lengths[s]%total;allocated+=samples[s]-1;
    }
    while(allocated<CLOUD){
        unsigned best=0;for(unsigned s=1;s<ink->strokes;s++)if(remainders[s]>remainders[best])best=s;
        samples[best]++;remainders[best]=0;allocated++;
    }
    unsigned n=0;
    for(unsigned s=0;s<ink->strokes;s++)for(unsigned k=0;k<samples[s];k++){
        if(samples[s]==1){out[n++]=normalized[starts[s]];continue;}
        unsigned target=k*lengths[s]/(samples[s]-1),walked=0;struct point p=normalized[ends[s]];
        for(unsigned i=starts[s]+1;i<=ends[s];i++){
            unsigned len=length(normalized[i-1],normalized[i]);
            if(len && target<=walked+len){
                unsigned at=target-walked;
                p.x=normalized[i-1].x+(normalized[i].x-normalized[i-1].x)*(int)at/(int)len;
                p.y=normalized[i-1].y+(normalized[i].y-normalized[i-1].y)*(int)at/(int)len;break;
            }
            walked+=len;
        }
        out[n++]=p;
    }
    return n==CLOUD;
}
unsigned uihw_examples(void){return ARRAY_SIZE(ink_shapes);}
char uihw_example(unsigned index,struct ui_ink *out)
{
    ui_ink_clear(out);if(index>=ARRAY_SIZE(ink_shapes))return 0;
    const char *p=ink_shapes[index].path;bool first=true;
    while(*p){
        if(*p=='/'){out->drawing=false;first=true;p++;continue;}
        if(*p==' '){p++;continue;}
        int x=(p[0]-'0')*10,y=(p[1]-'0')*10;p+=2;
        if(first){ui_ink_begin(out,x,y);first=false;}else ui_ink_move(out,x,y);
    }
    out->drawing=false;return ink_shapes[index].ch;
}
static unsigned distance(const struct point a[CLOUD],const struct point b[CLOUD])
{
    unsigned sum=0;
    for(unsigned i=0;i<CLOUD;i++){
        unsigned ab=~0u,ba=~0u;
        for(unsigned j=0;j<CLOUD;j++){
            int x=a[i].x-b[j].x,y=a[i].y-b[j].y;ab=MIN(ab,(unsigned)(x*x+y*y));
            x=b[i].x-a[j].x;y=b[i].y-a[j].y;ba=MIN(ba,(unsigned)(x*x+y*y));
        }
        sum+=ab+ba;
    }
    return sum/(CLOUD*2);
}
unsigned ui_ink_recognize(const struct ui_ink *ink,bool numbers,struct ui_ink_choice out[UI_INK_CHOICES])
{
    struct point points[CLOUD];if(!cloud(ink,points))return 0;
    if(!prepared){
        struct ui_ink example;
        for(unsigned i=0;i<ARRAY_SIZE(ink_shapes);i++){uihw_example(i,&example);cloud(&example,templates[i]);}
        prepared=true;
    }
    unsigned scores[26];for(unsigned i=0;i<26;i++)scores[i]=~0u;
    unsigned count=numbers?10:26;char base=numbers?'0':'a';
    for(unsigned i=0;i<ARRAY_SIZE(ink_shapes);i++){
        char c=ink_shapes[i].ch;if(c<base || c>=base+(int)count)continue;
        unsigned d=distance(points,templates[i]);scores[(unsigned)(c-base)]=MIN(scores[(unsigned)(c-base)],d);
    }
    unsigned n=0;
    while(n<UI_INK_CHOICES){
        unsigned best=0;for(unsigned i=1;i<count;i++)if(scores[i]<scores[best])best=i;
        if(scores[best]>1800)break;
        out[n++]=(struct ui_ink_choice){(char)(base+best),scores[best]};scores[best]=~0u;
    }
    return n;
}
