/*
 * Native check of libmedia and the Helix decoder: decodes a file the way
 * the audio server and the Video app do and writes the PCM as raw S16LE.
 *
 *   mediatest file.mp3|file.mp2|file.wav|file.avi|file.mpg out.raw [rate [frames.rgb565]]
 *
 * With a rate (0 for none), the output goes through the resampler (stereo
 * kept). For an AVI it also decodes every video frame and prints the count
 * and size. An MPEG-1 program stream goes through the Video app's mpeg.c,
 * which writes every frame as RGB565 to frames.rgb565 if given.
 * Compare against `ffmpeg -i file -f s16le -` (tests/mediatest.py).
 */

#include <glaze/fs.h>
#include <glaze/media.h>
#include <mp3dec.h>
#include "../apps/video/mpeg.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include <stb_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *file;
static uint32_t file_size;
static FILE *out;
static struct media_resampler rs;
static bool resample;

static int rd(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    (void)ctx;
    if (off >= file_size)
        return 0;
    len = MIN(len, file_size - off);
    memcpy(buf, file + off, len);
    return (int)len;
}

static void emit(const int16_t *pcm, int frames, int ch)
{
    static int16_t tmp[8192];

    if (!resample) {
        fwrite(pcm, 2u * ch, (size_t)frames, out);
        return;
    }
    while (frames > 0) {
        int used, n = media_rs_run(&rs, pcm, frames, tmp, 4096, &used);
        fwrite(tmp, 2u * rs.out_ch, (size_t)n, out);
        pcm += used * ch;
        frames -= used;
    }
}

static int do_mp3(uint32_t rate)
{
    struct media_mp3_info mi;
    MP3FrameInfo fi;
    static int16_t pcm[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];

    if (media_mp3_probe(rd, NULL, file_size, &mi))
        return fprintf(stderr, "not an MP3\n"), 1;
    printf("mp3: %u Hz, %u ch, %u kbps, %u ms, title '%s' artist '%s', seek(5000)=%u\n",
           mi.rate, mi.channels, mi.bitrate, mi.duration_ms, mi.title, mi.artist,
           media_mp3_seek_offset(&mi, 5000));
    if (rate)
        media_rs_init(&rs, mi.rate, (int)mi.channels, rate, (int)mi.channels);
    HMP3Decoder dec = MP3InitDecoder();
    unsigned char *p = file + mi.audio_off;
    int left = (int)(mi.audio_end - mi.audio_off), frames = 0;
    while (left > 0) {
        int sync = MP3FindSyncWord(p, left);
        if (sync < 0)
            break;
        p += sync;
        left -= sync;
        int err = MP3Decode(dec, &p, &left, pcm, 0);
        if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            if (err == ERR_MP3_INDATA_UNDERFLOW)
                break;
            continue;
        }
        if (err) {
            p++, left--;
            continue;
        }
        MP3GetLastFrameInfo(dec, &fi);
        emit(pcm, fi.outputSamps / fi.nChans, fi.nChans);
        frames++;
    }
    MP3FreeDecoder(dec);
    printf("mp3: %d frames\n", frames);
    return 0;
}

/* An MPEG audio Layer II elementary stream, frame by frame. */
static int do_mp2(uint32_t rate)
{
    static struct media_mp2 d;
    static int16_t pcm[MEDIA_MP2_FRAME_SAMPLES * 2];
    struct media_mp2_info h;
    uint32_t at = 0;
    int frames = 0, first = 1;

    media_mp2_init(&d);
    while (at + 4 <= file_size) {
        if (media_mp2_header(file + at, file_size - at, &h) || at + h.frame_len > file_size) {
            at++;
            continue;
        }
        if (first) {
            printf("mp2: %u Hz, %u ch, %u kbps%s\n", h.rate, h.channels, h.bitrate, h.lsf ? " (LSF)" : "");
            if (rate)
                media_rs_init(&rs, h.rate, h.channels, rate, h.channels);
            first = 0;
        }
        int n = media_mp2_decode(&d, file + at, h.frame_len, pcm);
        if (n < 0)
            return fprintf(stderr, "bad frame at %u\n", at), 1;
        emit(pcm, n, h.channels);
        at += h.frame_len;
        frames++;
    }
    printf("mp2: %d frames\n", frames);
    return frames ? 0 : 1;
}

static void decode_audio(const struct media_audio_fmt *f, uint32_t off, uint32_t len)
{
    static int16_t pcm[16384];
    uint8_t *p = file + off;

    while (len > 0) {
        uint32_t unit = f->format == MEDIA_FMT_IMA ? MIN(len, f->block_align)
                                                   : MIN(len, 4096u / f->block_align * f->block_align);
        int n = media_decode(f, p, unit, pcm);
        emit(pcm, n, f->channels);
        p += unit;
        len -= unit;
    }
}

/* mpeg.c reads through the file server; here, the file in memory. */
static uint32_t fake_pos;

int fs_seek(int fd, int32_t offset, int whence)
{
    (void)fd, (void)whence;
    fake_pos = (uint32_t)offset;
    return 0;
}

int fs_read(int fd, void *buf, uint32_t len)
{
    (void)fd;
    int n = rd(NULL, fake_pos, buf, len);
    fake_pos += (uint32_t)(n > 0 ? n : 0);
    return n;
}

static const char *frames_out;

static int do_mpg(uint32_t rate)
{
    struct mpeg_info mi;
    static int16_t pcm[MEDIA_MP2_FRAME_SAMPLES * 2];
    uint32_t ms, last = 0;
    int frames = 0, aframes = 0, bad = 0;

    if (!mpeg_open(3, file_size, &mi))
        return fprintf(stderr, "not an MPEG-1 program stream\n"), 1;
    printf("mpg: %ux%u, %u us/frame, %u ms, audio %d (%u Hz %u ch)\n", mi.width, mi.height,
           mi.frame_us, mi.duration_ms, mi.has_audio, mi.rate, mi.channels);
    FILE *vf = frames_out ? fopen(frames_out, "wb") : NULL;
    uint16_t *px = malloc(mi.width * mi.height * 2);
    /* All the video first, which demuxes all the sound into mpeg.c's
     * queue only as far as it holds; so alternate, as the app does. */
    if (rate && mi.has_audio)
        media_rs_init(&rs, mi.rate, mi.channels, rate, mi.channels);
    bool vend = false, aend = !mi.has_audio;
    while (!vend || !aend) {
        if (!vend) {
            if (mpeg_next_frame(&ms)) {
                if (frames && ms <= last)
                    bad++;
                last = ms;
                mpeg_convert(px);
                if (vf)
                    fwrite(px, 2, mi.width * mi.height, vf);
                frames++;
            } else {
                vend = true;
            }
        }
        /* Keep the sound up with the picture, as the player's queue does. */
        while (!aend && (vend || aframes * 1000ull * MEDIA_MP2_FRAME_SAMPLES / mi.rate <= last + 800)) {
            int n = mpeg_audio(pcm, &ms);
            if (n <= 0) {
                aend = true;
                break;
            }
            emit(pcm, n, mi.channels);
            aframes++;
        }
    }
    printf("mpg: %d frames (last at %u ms), %d audio frames%s\n", frames, last, aframes,
           bad ? ", TIMES OUT OF ORDER" : "");
    /* Seeks from where the last left off, as the player does: write the
     * frame shown and its time to frames.rgb565.seek for the comparison. */
    static const uint32_t seeks[] = { 500, 50, 90, 25, 66, 0, 75 };   /* % of the duration */
    FILE *sf = NULL;
    if (frames_out) {
        char name[512];
        snprintf(name, sizeof(name), "%s.seek", frames_out);
        sf = fopen(name, "wb");
    }
    for (size_t k = 0; k < sizeof(seeks) / sizeof(seeks[0]); k++) {
        uint32_t want = seeks[k] == 500 ? mi.duration_ms / 2 : mi.duration_ms * seeks[k] / 100, shown;
        if (!mpeg_seek(want, &shown)) {
            printf("mpg: seek(%u) FAILED\n", want);
            bad++;
            continue;
        }
        mpeg_convert(px);
        printf("mpg: seek(%u) shows %u ms\n", want, shown);
        if (sf) {
            fwrite(&shown, 4, 1, sf);
            fwrite(px, 2, mi.width * mi.height, sf);
        }
        /* and play on a little, as after a resume */
        for (int i = 0; i < 5 && mpeg_next_frame(&ms); i++)
            ;
        int16_t tmp[MEDIA_MP2_FRAME_SAMPLES * 2];
        if (mi.has_audio && mpeg_audio(tmp, &ms) <= 0 && want + 1000 < mi.duration_ms) {
            printf("mpg: no sound after seek(%u)\n", want);
            bad++;
        }
    }
    if (sf)
        fclose(sf);
    if (vf)
        fclose(vf);
    free(px);
    mpeg_close();
    return frames && !bad ? 0 : 1;
}

static int do_wav(uint32_t rate)
{
    struct media_wav w;

    if (media_wav_open(rd, NULL, file_size, &w))
        return fprintf(stderr, "unsupported WAV\n"), 1;
    printf("wav: fmt 0x%x, %u Hz, %u ch, %u frames\n", w.fmt.format, w.fmt.rate, w.fmt.channels,
           media_fmt_frames(&w.fmt, w.data_len));
    if (rate)
        media_rs_init(&rs, w.fmt.rate, w.fmt.channels, rate, w.fmt.channels);
    decode_audio(&w.fmt, w.data_off, w.data_len);
    return 0;
}

static int do_avi(uint32_t rate)
{
    struct media_avi a;
    static uint8_t fixed[1 << 20];
    int ok = 0, w = 0, h = 0;

    if (media_avi_open(rd, NULL, file_size, &a))
        return fprintf(stderr, "unsupported AVI\n"), 1;
    printf("avi: %ux%u, %u us/frame, mjpeg %d, %u video chunks, audio %d (fmt 0x%x %u Hz %u ch), "
           "%u audio chunks, %u audio frames\n", a.width, a.height, a.usec_per_frame, a.mjpeg,
           a.nvideo, a.has_audio, a.audio.format, a.audio.rate, a.audio.channels, a.naudio,
           a.audio_frames);
    for (uint32_t i = 0; i < a.nvideo; i++) {
        const uint8_t *j = file + a.video[i].off;
        uint32_t len = a.video[i].len;
        int n = media_jpeg_add_dht(j, len, fixed);
        if (n > 0)
            j = fixed, len = (uint32_t)n;
        int comp;
        uint8_t *px = stbi_load_from_memory(j, (int)len, &w, &h, &comp, 3);
        if (px)
            ok++;
        stbi_image_free(px);
    }
    printf("avi: %d/%u frames decoded, %dx%d\n", ok, a.nvideo, w, h);
    if (a.has_audio) {
        if (rate)
            media_rs_init(&rs, a.audio.rate, a.audio.channels, rate, a.audio.channels);
        for (uint32_t i = 0; i < a.naudio; i++)
            decode_audio(&a.audio, a.audio_chunks[i].off, a.audio_chunks[i].len);
    }
    int rv = ok == (int)a.nvideo ? 0 : 1;
    media_avi_close(&a);
    return rv;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return fprintf(stderr, "usage: mediatest file out.raw [rate]\n"), 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return perror(argv[1]), 1;
    fseek(f, 0, SEEK_END);
    file_size = (uint32_t)ftell(f);
    rewind(f);
    file = malloc(file_size);
    if (fread(file, 1, file_size, f) != file_size)
        return 1;
    fclose(f);
    out = fopen(argv[2], "wb");
    uint32_t rate = argc > 3 ? (uint32_t)atoi(argv[3]) : 0;
    frames_out = argc > 4 ? argv[4] : NULL;
    resample = rate != 0;
    const char *ext = strrchr(argv[1], '.');
    int rv = !ext ? 2 : !strcasecmp(ext, ".mp3") ? do_mp3(rate) : !strcasecmp(ext, ".mp2") ? do_mp2(rate) : !strcasecmp(ext, ".mpg") ? do_mpg(rate) : !strcasecmp(ext, ".wav") ? do_wav(rate)
           : !strcasecmp(ext, ".avi") ? do_avi(rate) : 2;
    fclose(out);
    return rv;
}
