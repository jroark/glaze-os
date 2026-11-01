#include "../../packages/tetris/game.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static Game g;
static void empty(int p){game_new(&g,123,0);memset(g.board,0,sizeof(g.board));g.piece=p;g.x=3;g.y=3;g.rotation=0;}
int main(void)
{
    /* Many seeds: first seven pieces form a bag, hold cannot be repeated. */
    for(unsigned seed=1;seed<200;seed++){
        game_new(&g,seed,0);unsigned mask=0;
        for(int n=0;n<7;n++){mask|=1u<<g.piece;game_drop(&g);memset(g.board,0,sizeof(g.board));g.over=0;}
        assert(mask==127);
    }
    empty(5);int p=g.piece;game_hold(&g);assert(g.hold==p&&g.held);Game copy=g;game_hold(&g);assert(!memcmp(&g,&copy,sizeof(g)));
    game_drop(&g);assert(!g.held);game_hold(&g);assert(g.piece==p);
    empty(5);for(int i=0;i<4;i++)assert(game_rotate(&g));assert(g.rotation==0);
    /* T at left wall, rotation from R to 2 requires a kick right. */
    empty(5);g.rotation=1;g.x=-1;assert(game_fits(&g,g.x,g.y,1));assert(game_rotate(&g));assert(g.x==0);
    empty(0);g.rotation=1;g.x=-2;assert(game_fits(&g,g.x,g.y,1));assert(game_rotate(&g));assert(g.x==0);
    empty(3);assert(!game_move(&g,-10,0,false));int y=game_ghost(&g);assert(y==20);game_drop(&g);assert(g.board[21][4]==4&&g.score==34);
    for(int lines=1;lines<=4;lines++){
        empty(0);g.rotation=1;g.x=2;g.y=18;
        for(int j=TH-lines;j<TH;j++)for(int x=0;x<TW;x++)g.board[j][x]=(x==4?0:2);
        static const unsigned scores[]={0,100,300,500,800};
        game_drop(&g);assert(g.lines==(unsigned)lines&&g.score==scores[lines]);
    }
    empty(3);g.lines=9;g.y=20;for(int x=0;x<TW;x++)if(x!=4&&x!=5)g.board[21][x]=1;
    game_drop(&g);assert(game_level(&g)==2&&game_speed(&g)==740);
    g.lines=1000;assert(game_speed(&g)==80);
    empty(3);g.y=20;game_tick(&g,499);assert(g.lock==499);assert(game_move(&g,1,0,false));assert(g.lock==0&&g.resets==1);
    for(int i=0;i<20;i++)game_move(&g,i%2?1:-1,0,false);
    assert(g.resets==15);game_tick(&g,499);game_move(&g,-1,0,false);assert(g.lock==499);game_tick(&g,1);assert(g.y==0);
    empty(3);g.y=0;g.board[2][4]=1;game_tick(&g,500);assert(g.over);
    empty(5);game_move(&g,0,1,true);assert(g.score==1&&g.best==1);
    uint8_t bytes[SAVE_BYTES+1];game_encode(&g,bytes);Game restored;
    assert(game_decode(&restored,bytes,SAVE_BYTES));assert(!memcmp(&g,&restored,sizeof(g)));
    assert(!game_decode(&restored,bytes,SAVE_BYTES-1));assert(!game_decode(&restored,bytes,SAVE_BYTES+1));
    bytes[20]^=1;assert(!game_decode(&restored,bytes,SAVE_BYTES));
    g.board[5][0]=9;game_encode(&g,bytes);assert(!game_decode(&restored,bytes,SAVE_BYTES));
    /* Spawn collision after hold, floor kick, and arbitrary valid play. */
    empty(5);g.hold=3;g.board[0][4]=1;game_hold(&g);assert(g.over);
    empty(5);g.y=20;assert(game_fits(&g,g.x,g.y,0));assert(game_rotate(&g));assert(g.y<20);
    empty(5);g.next=9;game_encode(&g,bytes);assert(!game_decode(&restored,bytes,SAVE_BYTES));
    for(unsigned seed=1;seed<=50;seed++){
        game_new(&g,seed,100);
        for(int i=0;i<200&&!g.over;i++){
            switch(i%6){case 0:game_rotate(&g);break;case 1:game_move(&g,-1,0,false);break;
            case 2:game_hold(&g);break;case 3:game_move(&g,1,0,false);break;
            case 4:game_tick(&g,1000);break;case 5:game_drop(&g);break;}
            game_encode(&g,bytes);assert(game_decode(&restored,bytes,SAVE_BYTES));
            if(!g.over)assert(game_fits(&g,g.x,g.y,g.rotation));
        }
    }
    puts("tetris: all tests passed");
}
