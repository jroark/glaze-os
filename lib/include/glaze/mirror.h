/* Bounded RGB565 screen-delta stream inside the development protocol. */
#ifndef GLAZE_MIRROR_H
#define GLAZE_MIRROR_H
#define GM_WIDTH 240u
#define GM_HEIGHT 320u
#define GM_TILE 16u
#define GM_COLUMNS (GM_WIDTH / GM_TILE)
#define GM_TILES (GM_COLUMNS * (GM_HEIGHT / GM_TILE))
#define GM_RECORD (4u + GM_TILE * GM_TILE * 2u)
#define GM_FORCE 1u
#define GM_INPUT_BATCH 32u
/* NEXT: LE32 flags -> six LE32s: width, height, tile size, changed count,
 * stream size, whole-frame CRC32. First NEXT must force a full frame.
 * READ: LE32 offset -> <=GU_PAYLOAD bytes from an immutable snapshot.
 * Each record is LE32 tile index followed by 16x16 little-endian RGB565.
 * Read all records before NEXT. A lost/incomplete frame requires FORCE.
 * INPUT: 1..32 eight-byte records: LE16 type/code, LE16 signed x/y.
 * CLOSE: free the snapshot, reset the baseline, release held input. */
#endif
