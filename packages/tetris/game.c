#include "game.h"
#include <string.h>
/* Spawn orientations in SRS bounding boxes; all rotations share their pivot. */
static const uint16_t shapes[7] = {0x00f0,0x0071,0x0074,0x0066,0x0036,0x0072,0x0063};
bool game_cell(int p,int r,int x,int y)
{
    if(p<0||p>6||x<0||x>3||y<0||y>3) return false;
    int n=p==0?4:3;
    if(p==3) r=0;
    for(int i=0;i<r;i++){int t=x; x=y; y=n-1-t;}
    return x>=0&&y>=0&&x<n&&y<n&&(shapes[p] & (1u<<(y*4+x)));
}
bool game_fits(const Game *g,int x,int y,int r)
{
    for(int j=0;j<4;j++)for(int i=0;i<4;i++)if(game_cell(g->piece,r,i,j)){
        int xx=x+i,yy=y+j;
        if(xx<0||xx>=TW||yy<0||yy>=TH||g->board[yy][xx])return false;
    }
    return true;
}
static uint32_t random32(Game *g){g->rng^=g->rng<<13;g->rng^=g->rng>>17;g->rng^=g->rng<<5;return g->rng;}
static int take(Game *g)
{
    if(g->bag_pos==7){
        for(int i=0;i<7;i++)g->bag[i]=i;
        for(int i=6;i>0;i--){int j=random32(g)%(i+1);uint8_t t=g->bag[i];g->bag[i]=g->bag[j];g->bag[j]=t;}
        g->bag_pos=0;
    }
    return g->bag[g->bag_pos++];
}
static void spawn(Game *g,int p)
{
    g->piece=p;g->x=3;g->y=0;g->rotation=0;g->gravity=0;g->lock=0;g->resets=0;
    if(!game_fits(g,g->x,g->y,0))g->over=1;
}
void game_new(Game *g,uint32_t seed,uint32_t best)
{
    memset(g,0,sizeof(*g));g->rng=seed?seed:1;g->best=best;g->hold=-1;g->bag_pos=7;
    int p=take(g);g->next=take(g);spawn(g,p);
}
unsigned game_level(const Game *g){return 1+g->lines/10;}
unsigned game_speed(const Game *g){unsigned level=game_level(g);return level>=13?80:800-60*(level-1);}
static void points(Game *g,unsigned n){g->score+=n;if(g->score>g->best)g->best=g->score;}
static void reset_lock(Game *g,bool grounded)
{
    if(grounded&&g->resets<15){g->lock=0;g->resets++;}
}
bool game_move(Game *g,int dx,int dy,bool soft)
{
    if(g->over||!game_fits(g,g->x+dx,g->y+dy,g->rotation))return false;
    bool grounded=!game_fits(g,g->x,g->y+1,g->rotation);
    g->x+=dx;g->y+=dy;
    if(dx)reset_lock(g,grounded);
    if(soft&&dy>0)points(g,dy);
    return true;
}
/* SRS clockwise kick offsets in screen coordinates (+y down). */
static const int8_t kicks[4][5][2]={
    {{0,0},{-1,0},{-1,-1},{0,2},{-1,2}},
    {{0,0},{1,0},{1,1},{0,-2},{1,-2}},
    {{0,0},{1,0},{1,-1},{0,2},{1,2}},
    {{0,0},{-1,0},{-1,1},{0,-2},{-1,-2}}
};
static const int8_t ikicks[4][5][2]={
    {{0,0},{-2,0},{1,0},{-2,1},{1,-2}},
    {{0,0},{-1,0},{2,0},{-1,-2},{2,1}},
    {{0,0},{2,0},{-1,0},{2,-1},{-1,2}},
    {{0,0},{1,0},{-2,0},{1,2},{-2,-1}}
};
bool game_rotate(Game *g)
{
    if(g->over||g->piece==3)return false;
    bool grounded=!game_fits(g,g->x,g->y+1,g->rotation);
    int r=(g->rotation+1)%4;
    for(int i=0;i<5;i++){
        const int8_t *k=g->piece==0?ikicks[g->rotation][i]:kicks[g->rotation][i];
        if(game_fits(g,g->x+k[0],g->y+k[1],r)){
            g->x+=k[0];g->y+=k[1];g->rotation=r;reset_lock(g,grounded);return true;
        }
    }
    return false;
}
void game_hold(Game *g)
{
    if(g->over||g->held)return;
    int p=g->piece;
    if(g->hold<0){spawn(g,g->next);g->next=take(g);}else spawn(g,g->hold);
    g->hold=p;g->held=1;
}
int game_ghost(const Game *g){int y=g->y;while(game_fits(g,g->x,y+1,g->rotation))y++;return y;}
static void lock_piece(Game *g)
{
    for(int j=0;j<4;j++)for(int i=0;i<4;i++)if(game_cell(g->piece,g->rotation,i,j)){
        g->board[g->y+j][g->x+i]=g->piece+1;
        if(g->y+j<2)g->over=1;
    }
    if(g->over)return;
    unsigned cleared=0;
    for(int y=TH-1;y>=0;y--){
        bool full=true;for(int x=0;x<TW;x++)if(!g->board[y][x])full=false;
        if(full){for(int j=y;j>0;j--)memcpy(g->board[j],g->board[j-1],TW);memset(g->board[0],0,TW);cleared++;y++;}
    }
    static const unsigned award[]={0,100,300,500,800};
    points(g,award[cleared]*game_level(g));g->lines+=cleared;g->held=0;
    spawn(g,g->next);g->next=take(g);
}
void game_drop(Game *g){if(!g->over){int y=game_ghost(g);points(g,2*(y-g->y));g->y=y;lock_piece(g);}}
void game_tick(Game *g,uint32_t ms)
{
    /* Bound work after stalls. UI never catches up time spent in the background. */
    if(ms>1000)ms=1000;
    while(ms--&&!g->over){
        if(!game_fits(g,g->x,g->y+1,g->rotation)){if(++g->lock>=500)lock_piece(g);}
        else if(++g->gravity>=game_speed(g)){g->gravity=0;game_move(g,0,1,false);}
    }
}
static uint32_t checksum(const uint8_t *p,unsigned n){uint32_t h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h;}
void game_encode(const Game *g,uint8_t *out)
{
    uint32_t header[3]={0x31544554,sizeof(Game),0};
    header[2]=checksum((const uint8_t *)g,sizeof(*g));memcpy(out,header,12);memcpy(out+12,g,sizeof(*g));
}
bool game_decode(Game *g,const uint8_t *in,unsigned size)
{
    uint32_t h[3];Game t;
    if(size!=SAVE_BYTES)return false;
    memcpy(h,in,12);memcpy(&t,in+12,sizeof(t));
    if(h[0]!=0x31544554||h[1]!=sizeof(Game)||h[2]!=checksum(in+12,sizeof(t)))return false;
    if(t.piece<0||t.piece>6||t.next<0||t.next>6||t.hold< -1||t.hold>6||t.rotation<0||t.rotation>3||
       t.x< -3||t.x>9||t.y<0||t.y>=TH||t.bag_pos<0||t.bag_pos>7||!t.rng||t.held>1||t.over>1||
       t.resets>15||t.lock>500||t.gravity>800||t.lines>1000000||t.best<t.score)return false;
    unsigned mask=0;for(int i=0;i<7;i++){if(t.bag[i]>6)return false;mask|=1u<<t.bag[i];}
    if(mask!=127)return false;
    for(int y=0;y<TH;y++)for(int x=0;x<TW;x++)if(t.board[y][x]>7)return false;
    if(!t.over&&!game_fits(&t,t.x,t.y,t.rotation))return false;
    *g=t;return true;
}
