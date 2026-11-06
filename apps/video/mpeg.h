/*
 * MPEG-1 program streams (.mpg): MPEG-1 video through pl_mpeg, MPEG audio
 * Layer II through libmedia (media_mp2_*). One file is open at a time.
 *
 * Video decodes in order, one frame after another, so the player can't
 * jump to any frame the way it does with Motion-JPEG: mpeg_seek() goes back
 * to the intra frame at or before the time and decodes forward from there,
 * and a player that has fallen behind drops B frames, which nothing
 * depends on, with mpeg_skip_b().
 */
#ifndef VIDEO_MPEG_H
#define VIDEO_MPEG_H

#include <glaze/types.h>

struct mpeg_info {
    uint32_t width, height;
    uint32_t frame_us;                  /* 1 / frame rate */
    uint32_t duration_ms;
    bool has_audio;
    uint32_t rate;                      /* audio */
    uint16_t channels;
};

/* Opens the program stream in `fd` (`size` bytes) and reads its headers;
 * the first frame is next. */
bool mpeg_open(int fd, uint32_t size, struct mpeg_info *mi);
void mpeg_close(void);

/* Decodes the next frame; false at the end. *ms is its time. The frame is
 * kept for mpeg_convert() until the next decode. */
bool mpeg_next_frame(uint32_t *ms);
/* Skips the next frame instead, if it is a B frame, and adds a frame's time
 * to *next_ms. */
bool mpeg_skip_b(uint32_t *next_ms);
/* Writes the last decoded frame as RGB565 (width * height). */
void mpeg_convert(uint16_t *px);

/* Goes to `ms`: decodes from the intra frame before it up to the frame
 * showing at `ms` (*shown_ms), and restarts the sound at the first audio
 * frame from there. */
bool mpeg_seek(uint32_t ms, uint32_t *shown_ms);

/* Decodes the next audio frame (MEDIA_MP2_FRAME_SAMPLES frames of
 * mi->channels S16 samples); returns the frames, 0 at the end. *ms is when
 * it plays. */
int mpeg_audio(int16_t *pcm, uint32_t *ms);

/* Reads a little more of the file ahead, if the read-ahead is short. */
void mpeg_prefetch(void);

#endif
