#include "game.h"
#include <glaze/ui.h>
#include <glaze/fs.h>
#include <glaze/sys.h>
#include <stdio.h>
#include <string.h>
static Game game;
static char path[FS_PATH_MAX],temp[FS_PATH_MAX];
static bool paused,confirm,save_failed;
static int held_key=-1,selection;
static uint32_t key_at;
static const uint32_t colors[]={0,0x42D9E8,0x507EF0,0xF2A547,0xF5D85B,0x69CE8A,0xB88BF0,0xF07482};
static void save(void)
{
    uint8_t bytes[SAVE_BYTES];game_encode(&game,bytes);
    save_failed=fs_write_file(temp,bytes,sizeof(bytes))!=sizeof(bytes);
    if(!save_failed)save_failed=fs_rename(temp,path)<0;
}
static void clear_input(void){held_key=-1;}
static void block(int x,int y,int p,bool ghost)
{
    uint32_t c=colors[p];
    gfx_fill(&ui.surf,gfx_r(x,y,9,9),c);
    gfx_fill(&ui.surf,gfx_r(x+1,y+1,7,7),ghost?0x182539:c);
    if(!ghost)gfx_fill_alpha(&ui.surf,gfx_r(x+1,y+1,7,2),0xFFFFFF,80);
}
static void text(int x,int y,const char *s){gfx_text(&ui.surf,x,y,ui_font(FONT_B14),s,0xE5ECF5,255);}
static void preview(int p,int y)
{
    if(p<0)return;
    for(int j=0;j<4;j++)for(int i=0;i<4;i++)if(game_cell(p,0,i,j))block(144+i*10,y+j*10,p+1,false);
}
static void draw(void)
{
    gfx_surface *s=&ui.surf;
    gfx_fill(s,gfx_r(0,0,UI_W,UI_H),0x101B2B);
    ui_draw_navbar(s,"Tetris",NULL,NULL,0);
    gfx_fill(s,gfx_r(13,41,104,202),0x40536D);
    gfx_fill(s,gfx_r(15,42,100,200),0x182539);
    for(int y=2;y<TH;y++)for(int x=0;x<TW;x++){
        int p=game.board[y][x];
        if(p)block(15+x*10,42+(y-2)*10,p,false);
    }
    if(!game.over){
        int ghost=game_ghost(&game);
        for(int j=0;j<4;j++)for(int i=0;i<4;i++)if(game_cell(game.piece,game.rotation,i,j)){
            if(ghost+j>=2)block(15+(game.x+i)*10,42+(ghost+j-2)*10,game.piece+1,true);
            if(game.y+j>=2)block(15+(game.x+i)*10,42+(game.y+j-2)*10,game.piece+1,false);
        }
    }
    char b[32];
    text(130,43,"SCORE");snprintf(b,sizeof(b),"%u",game.score);text(130,60,b);
    text(130,80,"BEST");snprintf(b,sizeof(b),"%u",game.best);text(130,97,b);
    snprintf(b,sizeof(b),"Lines %u",game.lines);text(130,119,b);
    snprintf(b,sizeof(b),"Level %u",game_level(&game));text(130,138,b);
    text(130,161,"NEXT");preview(game.next,180);
    text(130,207,"HOLD");preview(game.hold,224);
    gfx_text_center(s,0,254,240,ui_font(FONT_B12),"Left / Right move   Down soft drop",0xA6BAD2,255);
    gfx_text_center(s,0,274,240,ui_font(FONT_B12),"Up turn   OK drop   ESC menu",0xA6BAD2,255);
    if(paused||game.over){
        gfx_fill_alpha(s,gfx_r(0,36,240,264),0x000000,185);
        gfx_round_rect(s,gfx_r(20,77,200,164),9,0x304763,255);
        gfx_text_center(s,20,88,200,ui_font(FONT_B20),confirm?"New game?":game.over?"Game over":"Paused",0xFFFFFF,255);
        const char *items[3]={"Resume","Hold piece","New Game"};
        int count=confirm?2:game.over?1:3;
        if(confirm){items[0]="Cancel";items[1]="Discard game";}
        else if(game.over)items[0]="New Game";
        for(int i=0;i<count;i++){
            if(i==selection)gfx_round_rect(s,gfx_r(33,121+i*30,174,26),4,0x52749C,255);
            gfx_text_center(s,20,127+i*30,200,ui_font(FONT_B14),items[i],0xFFFFFF,255);
        }
        gfx_text_center(s,20,219,200,ui_font(FONT_B12),"Up/Down select   OK choose",0xC8D8EB,255);
    }
    if(save_failed)gfx_text_center(s,0,282,240,ui_font(FONT_B12),"Could not save game",0xFFCF8A,255);
    ui_invalidate_all();ui_present();
}
static void action(int n)
{
    if(paused||game.over)return;
    switch(n){
    case 0:game_move(&game,-1,0,false);break;
    case 1:game_move(&game,1,0,false);break;
    case 2:game_rotate(&game);break;
    case 3:game_move(&game,0,1,true);break;
    case 4:game_drop(&game);save();break;
    case 5:game_hold(&game);break;
    }
}
static int key_action(int code)
{
    switch(code){case GZ_KEY_LEFT:return 0;case GZ_KEY_RIGHT:return 1;
    case GZ_KEY_UP:return 2;case GZ_KEY_DOWN:return 3;
    case GZ_KEY_OK:case GZ_KEY_ENTER:return 4;case GZ_KEY_TAB:return 5;default:return -1;}
}
static void toggle(void){paused=!paused;confirm=false;selection=0;clear_input();save();}
static void restart(void){game_new(&game,gz_time_ms(),game.best);paused=confirm=false;clear_input();save();}
int main(void)
{
    ui_init(SB_STATUS_GRAY);
    snprintf(path,sizeof(path),"%s/data",res_bundle());fs_mkdirs(path);
    snprintf(path,sizeof(path),"%s/data/state",res_bundle());
    snprintf(temp,sizeof(temp),"%s/data/state.tmp",res_bundle());
    uint8_t bytes[SAVE_BYTES+1];
    int size=fs_read_file(path,bytes,sizeof(bytes));
    if(size>0&&game_decode(&game,bytes,size))paused=true;
    else {game_new(&game,gz_time_ms(),0);save();}
    uint32_t last=gz_time_ms();draw();
    for(;;){
        struct gz_input_event e;
        int got=ui_next_event(&e,paused||game.over?-1:16);
        uint32_t now=gz_time_ms(),elapsed=now-last;last=now;
        if(paused||game.over)elapsed=0;
        if(got){
            if(e.type==GZ_EV_APP_DEACTIVATE){paused=true;confirm=false;selection=0;clear_input();save();continue;}
            if(e.type==GZ_EV_APP_ACTIVATE){paused=true;selection=0;clear_input();draw();continue;}

            if(e.type==GZ_EV_KEY_UP&&key_action(e.code)==held_key)held_key=-1;
            if(e.type==GZ_EV_KEY_DOWN){
                if(e.code==GZ_KEY_ESC){if(confirm){confirm=false;selection=0;}else if(!game.over)toggle();}
                else if(paused||game.over){
                    int count=confirm?2:game.over?1:3;
                    if(e.code==GZ_KEY_UP)selection=(selection+count-1)%count;
                    if(e.code==GZ_KEY_DOWN)selection=(selection+1)%count;
                    if(e.code==GZ_KEY_OK||e.code==GZ_KEY_ENTER){
                        if(game.over)restart();
                        else if(confirm){if(selection==1)restart();else {confirm=false;selection=0;}}
                        else if(selection==0)toggle();
                        else if(selection==1){game_hold(&game);toggle();}
                        else {confirm=true;selection=0;}
                    }
                }else{
                    int n=key_action(e.code);
                    if(n>=0&&n!=held_key){action(n);held_key=n;key_at=now+(n==3?40:180);}
                }
            }

        }
        if(!paused&&!game.over){
            if((held_key==0||held_key==1||held_key==3)&&(int32_t)(now-key_at)>=0){action(held_key);key_at=now+(held_key==3?40:70);}
            int old_pos=game.bag_pos,old_piece=game.piece;uint32_t old_score=game.score;
            game_tick(&game,elapsed);
            if(old_pos!=game.bag_pos||old_piece!=game.piece||old_score!=game.score||game.over)save();
        }
        if(got||(!paused&&!game.over))draw();
    }
}
