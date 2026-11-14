/*
 * web_inflate(), which http.c calls to decode gzip and deflate bodies, for a
 * file that holds stb_image's implementation (the browser's image.c, the App
 * Store's inflate.c): include this after stb_image.h. One copy, so every
 * program that links http.c decodes the same way.
 */
#ifndef BROWSER_INFLATE_H
#define BROWSER_INFLATE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

uint8_t *web_inflate(const uint8_t *in, int len, bool zlib_header, int cap, int hint, int *out_len, bool *cut);

/*
 * Inflates into at most cap bytes. stb's growing decoder has no limit, so a
 * small compressed body could ask for all the memory there is; this one
 * decodes into a fixed buffer and tries a larger one when that fills. With the
 * largest buffer full, or the stream ending early, *cut is set and what was
 * decoded is returned (the buffer starts zeroed, and text has no NULs).
 */
uint8_t *web_inflate(const uint8_t *in, int len, bool zlib_header, int cap, int hint, int *out_len, bool *cut)
{
    int size = hint > 0 && hint <= cap ? hint : len < cap / 4 ? len * 4 + 1024 : cap;

    *cut = false;
    if (size > cap)
        size = cap;
    for (;;) {
        uint8_t *out = calloc(1, (size_t)size + 1);
        if (!out)
            return NULL;
        int n = zlib_header ? stbi_zlib_decode_buffer((char *)out, size, (const char *)in, len)
                            : stbi_zlib_decode_noheader_buffer((char *)out, size, (const char *)in, len);
        if (n >= 0) {
            *out_len = n;
            return out;
        }
        if (size < cap) {
            free(out);
            size = size < cap / 4 ? size * 4 : cap;
            continue;
        }
        for (n = size; n && !out[n - 1]; n--)
            ;
        if (!n) {
            free(out);
            return NULL;
        }
        *out_len = n;
        *cut = true;
        return out;
    }
}

#endif
