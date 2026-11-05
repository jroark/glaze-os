/* JPEG headers for the Veo Photo Traveler CF camera (docs/hardware/VEO.md).
 *
 * The card's own encoder emits only a baseline scan: 4:2:2 YCbCr (Y H2V1),
 * Annex K luminance Huffman tables for all three components, and one of
 * two fixed quantisation sets chosen by bridge register 0xB2. A file is
 * this header followed by the scan bytes the FIFO delivered. */

#include <glaze/media.h>
#include <string.h>

/* The card's quantisation tables, in JPEG zig-zag order (T.81 Figure 5),
 * measured from Veo's Windows CE driver headers. "Fine" (0xB2 = 0x39) is
 * a flat 6 for both tables. */
static const uint8_t normal_luma[64] = {
     8,  5,  6,  7,  6,  5,  8,  7,  6,  7,  9,  8,  8,  9, 13, 21,
    14, 13, 12, 12, 13, 26, 17, 18, 14, 21, 32, 26, 32, 32, 28, 26,
    28, 28, 32, 37, 47, 39, 32, 34, 47, 34, 28, 28, 43, 57, 43, 47,
    51, 51, 51, 57, 51, 32, 39, 57, 64, 57, 51, 64, 47, 51, 51, 51,
};
static const uint8_t normal_chroma[64] = {
     8,  9,  9, 13, 11, 13, 23, 14, 14, 28, 51, 34, 28, 34, 51, 51,
    51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51,
    51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51,
    51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51,
};

static const uint16_t widths[4] = {0, 640, 320, 160};
static const uint16_t heights[4] = {0, 480, 240, 120};

int media_veo_jpeg_header(uint8_t *out, uint32_t size, unsigned mode, bool fine)
{
    static const uint8_t jfif[] = {
        0xFF, 0xD8,                                     /* SOI */
        0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0,  /* APP0, JFIF 1.02 */
        1, 2, 0, 0, 1, 0, 1, 0, 0,
    };
    static const uint8_t sos[] = {
        0xFF, 0xDA, 0x00, 0x0C, 3, 1, 0x00, 2, 0x00, 3, 0x00, 0, 63, 0,
    };
    uint32_t n = 0;

    if (mode < 1 || mode > 3 || size < MEDIA_VEO_HEADER_SIZE)
        return -1;
    memcpy(out, jfif, sizeof(jfif)); n += sizeof(jfif);

    out[n++] = 0xFF; out[n++] = 0xDB; out[n++] = 0; out[n++] = 2 + 2 * 65;   /* DQT */
    out[n++] = 0;
    if (fine) memset(out + n, 6, 64); else memcpy(out + n, normal_luma, 64);
    n += 64;
    out[n++] = 1;
    if (fine) memset(out + n, 6, 64); else memcpy(out + n, normal_chroma, 64);
    n += 64;

    const uint8_t sof[] = {
        0xFF, 0xC0, 0x00, 0x11, 8,
        (uint8_t)(heights[mode] >> 8), (uint8_t)heights[mode],
        (uint8_t)(widths[mode] >> 8), (uint8_t)widths[mode],
        3, 1, 0x21, 0, 2, 0x11, 1, 3, 0x11, 1,
    };
    memcpy(out + n, sof, sizeof(sof)); n += sizeof(sof);
    memcpy(out + n, media_jpeg_standard_dht(), MEDIA_JPEG_DHT_SIZE); n += MEDIA_JPEG_DHT_SIZE;
    memcpy(out + n, sos, sizeof(sos)); n += sizeof(sos);
    return (int)n;
}
