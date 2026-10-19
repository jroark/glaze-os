/*
 * Native check of libmedia and the Helix decoder: decodes a file the way
 * the audio server and the Video app do and writes the PCM as raw S16LE.
 *
 *   mediatest file.mp3|file.wav|file.avi out.raw [rate]
 *
 * With a rate, the output goes through the resampler (stereo kept). For an
 * AVI it also decodes every video frame and prints the count and size.
 * Compare against `ffmpeg -i file -f s16le -` (tests/mediatest.py).
 */

#include <glaze/media.h>
#include <mp3dec.h>
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
    resample = rate != 0;
    const char *ext = strrchr(argv[1], '.');
    int rv = !ext ? 2 : !strcasecmp(ext, ".mp3") ? do_mp3(rate) : !strcasecmp(ext, ".wav") ? do_wav(rate)
           : !strcasecmp(ext, ".avi") ? do_avi(rate) : 2;
    fclose(out);
    return rv;
}
