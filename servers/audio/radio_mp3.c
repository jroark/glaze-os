#include "radio_mp3.h"
#include <string.h>

void radio_mp3_close(struct radio_mp3 *d)
{
    if (d->decoder) MP3FreeDecoder(d->decoder);
    d->decoder = 0;
    d->len = 0;
}
int radio_mp3_reset(struct radio_mp3 *d)
{
    radio_mp3_close(d);
    d->decoder = MP3InitDecoder();
    return d->decoder ? 0 : -1;
}
int radio_mp3_feed(struct radio_mp3 *d, const uint8_t *p, int n)
{
    int space = (int)sizeof(d->input) - d->len;
    if (n > space) n = space;
    memcpy(d->input + d->len, p, (size_t)n);
    d->len += n;
    return n;
}
int radio_mp3_frame(struct radio_mp3 *d, int16_t *out, MP3FrameInfo *info)
{
    int off = 0, frames = 0;
    while (d->len - off >= 4) {
        int sync = MP3FindSyncWord(d->input + off, d->len - off);
        if (sync < 0) { off = d->len - 1; break; }
        off += sync;
        if (d->len - off < 4) break;
        MP3FrameInfo next;
        if (MP3GetNextFrameInfo(d->decoder, &next, d->input + off) ||
            next.layer != 3 || next.bitrate <= 0 || next.samprate < 8000 ||
            next.samprate > 48000 || (next.nChans != 1 && next.nChans != 2)) {
            off++; continue;
        }
        unsigned char *p = d->input + off;
        int avail = d->len - off;
        int rv = MP3Decode(d->decoder, &p, &avail, out, 0);
        if (rv == ERR_MP3_INDATA_UNDERFLOW) {
            if (d->len - off < 2048) break;
            off++; continue;
        }
        if (rv == ERR_MP3_MAINDATA_UNDERFLOW) {
            int used = (int)(p - d->input);
            off = used > off ? used : off + 1;
            continue;
        }
        if (rv) { off++; continue; }
        off = (int)(p - d->input);
        MP3GetLastFrameInfo(d->decoder, info);
        frames = info->outputSamps / info->nChans;
        break;
    }
    if (off) {
        memmove(d->input, d->input + off, (size_t)(d->len - off));
        d->len -= off;
    }
    return frames;
}
