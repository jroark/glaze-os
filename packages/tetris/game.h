#ifndef TETRIS_GAME_H
#define TETRIS_GAME_H
#include <stdint.h>
#include <stdbool.h>
#define TW 10
#define TH 22
/* Rows 0..1 are hidden. IDs: I J L O S T Z. No platform dependencies. */
typedef struct {
    uint8_t board[TH][TW], bag[7];
    int32_t piece, rotation, x, y, next, hold, bag_pos;
    uint32_t rng, score, best, lines, gravity, lock;
    uint8_t held, over, resets, reserved;
} Game;
void game_new(Game *g, uint32_t seed, uint32_t best);
bool game_cell(int piece, int rotation, int x, int y);
bool game_fits(const Game *g, int x, int y, int rotation);
bool game_move(Game *g, int dx, int dy, bool soft);
bool game_rotate(Game *g);
void game_hold(Game *g);
void game_drop(Game *g);
void game_tick(Game *g, uint32_t ms);
int game_ghost(const Game *g);
unsigned game_level(const Game *g);
unsigned game_speed(const Game *g);
#define SAVE_BYTES (sizeof(Game) + 12)
void game_encode(const Game *g, uint8_t *out);
bool game_decode(Game *g, const uint8_t *in, unsigned size);
#endif
