/*
 * libmedia: integer-only audio and container code shared by the audio
 * server, the Video app and the host test (host/mediatest). MP3 decoding is
 * the vendored Helix decoder (third_party/helix-mp3, pub/mp3dec.h); JPEG
 * frames go through stb_image in the app.
 *
 * Everything reads through a callback so the same parsers work on the file
 * server's client (fs_read) and on host stdio.
 */
#ifndef GLAZE_MEDIA_H
#define GLAZE_MEDIA_H

#include <glaze/types.h>

/* Reads `len` bytes at `off`; returns the bytes read or a negative error. */
typedef int (*media_read_fn)(void *ctx, uint32_t off, void *buf, uint32_t len);

/* ---- WAV ----------------------------------------------------------------- */

#define MEDIA_FMT_PCM       1
#define MEDIA_FMT_IMA       0x11        /* IMA (DVI) ADPCM, WAV block layout */

struct media_audio_fmt {
    uint16_t format;                    /* MEDIA_FMT_* */
    uint16_t channels;                  /* 1 or 2 */
    uint32_t rate;
    uint16_t bits;                      /* PCM: 8 or 16 */
    uint16_t block_align;               /* bytes per PCM frame or ADPCM block */
    uint16_t block_frames;              /* frames per ADPCM block */
};

struct media_wav {
    struct media_audio_fmt fmt;
    uint32_t data_off, data_len;
};

/* Parses a RIFF/WAVE header; 0 or a negative error for unsupported files. */
int media_wav_open(media_read_fn rd, void *ctx, uint32_t file_size, struct media_wav *w);
/* Parses a WAVEFORMATEX in a fmt or strf chunk. */
int media_audio_fmt_parse(const uint8_t *p, uint32_t len, struct media_audio_fmt *f);

/* Frames per `bytes` of data, and the data offset of frame `frame` rounded
 * down to a decodable unit (a PCM frame or an ADPCM block). */
uint32_t media_fmt_frames(const struct media_audio_fmt *f, uint32_t bytes);
uint32_t media_fmt_offset(const struct media_audio_fmt *f, uint32_t frame);

/* Decodes one unit of `in` (any whole number of PCM frames, or one ADPCM
 * block) to interleaved S16; returns the frames produced. `out` must hold
 * block_frames * channels samples for ADPCM. */
int media_decode(const struct media_audio_fmt *f, const uint8_t *in, uint32_t len, int16_t *out);

/* ---- resampling ---------------------------------------------------------- */

/* The output rate the hardware supports nearest to `rate`. */
uint32_t media_hw_rate(uint32_t rate);

/* Linear interpolation in 16.16 fixed point, mono or stereo in and out
 * (stereo to mono averages, mono to stereo duplicates). */
struct media_resampler {
    uint32_t in_rate, out_rate;
    uint16_t in_ch, out_ch;
    uint32_t step;                      /* input frames per output frame, 16.16 */
    uint32_t frac;                      /* position between prev and next, 16.16 */
    int16_t prev[2];
    bool primed;
};

void media_rs_init(struct media_resampler *rs, uint32_t in_rate, int in_ch, uint32_t out_rate, int out_ch);
/* Converts up to `in_frames`, stopping when `out_max` frames are written;
 * returns the frames written and sets *in_used. */
int media_rs_run(struct media_resampler *rs, const int16_t *in, int in_frames,
                 int16_t *out, int out_max, int *in_used);

/* ---- MP3 ----------------------------------------------------------------- */

struct media_mp3_info {
    uint32_t audio_off;                 /* first byte after an ID3v2 tag */
    uint32_t audio_end;                 /* before an ID3v1 tag */
    uint32_t rate, channels, bitrate;   /* from the first frame */
    uint32_t duration_ms;
    uint32_t xing_frames;               /* 0 unless a Xing/Info header */
    uint8_t toc[100];                   /* Xing seek table, if has_toc */
    bool has_toc;
    char title[64], artist[64];         /* ID3v1 or ID3v2, Latin-1 */
};

int media_mp3_probe(media_read_fn rd, void *ctx, uint32_t file_size, struct media_mp3_info *mi);
/* File offset near `ms` into the stream; resynchronize to a frame there. */
uint32_t media_mp3_seek_offset(const struct media_mp3_info *mi, uint32_t ms);

/* ---- AVI ----------------------------------------------------------------- */

struct media_avi_chunk {
    uint32_t off;                       /* file offset of the chunk data */
    uint32_t len;
};

struct media_avi {
    uint32_t width, height;
    uint32_t usec_per_frame;
    bool mjpeg;
    bool has_audio;
    struct media_audio_fmt audio;
    uint32_t nvideo, naudio;
    struct media_avi_chunk *video;      /* malloc'd, from idx1 */
    struct media_avi_chunk *audio_chunks;
    uint32_t *audio_frame;              /* first audio frame of each chunk */
    uint32_t audio_frames;              /* total */
};

/* Parses the headers and idx1 (or walks movi when there is no index). */
int media_avi_open(media_read_fn rd, void *ctx, uint32_t file_size, struct media_avi *a);
void media_avi_close(struct media_avi *a);

/* ---- JPEG ---------------------------------------------------------------- */

/* Motion-JPEG frames may leave out the Huffman tables (the "AVI1" format),
 * expecting the standard ones from JPEG Annex K. If `in` has no DHT
 * segment, writes a copy with them inserted into `out` (in_len + MEDIA_JPEG_DHT_SIZE
 * bytes)
 * and returns its length; otherwise returns 0. */
#define MEDIA_JPEG_DHT_SIZE 420
int media_jpeg_add_dht(const uint8_t *in, uint32_t in_len, uint8_t *out);

#endif
