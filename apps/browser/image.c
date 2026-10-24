/*
 * Images: PNG, JPEG and GIF (first frame) through the vendored stb_image,
 * box-filtered down to the size they'll be shown at and stored as RGB565
 * plus an alpha plane when they have transparency.
 */

#include "net.h"
#include "web.h"

#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_ASSERT(x)          ((void)0)
#define STBI_MAX_DIMENSIONS     4096
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "stb_image.h"
#pragma GCC diagnostic pop
/* With STBI_NO_THREAD_LOCALS, stb declares three _thread functions it never
 * defines; that warning comes at the end of the file. */
#pragma GCC diagnostic ignored "-Wunused-function"

#define MAX_SOURCE_PIXELS   (1600u * 1200u)     /* decoded RGBA must fit in RAM */
#define IMAGE_BUDGET        (3u << 20)          /* document plus frames, decoded */
#define MAX_W               240

static uint32_t tree_image_bytes(struct page *p)
{
    uint32_t bytes = p->img_bytes;
    for (int i = 0; i < p->nframes; i++)
        if (p->frames[i].page) bytes += tree_image_bytes(p->frames[i].page);
    return bytes;
}

bool image_decode(struct page *p, struct image_slot *s, const uint8_t *data, int len)
{
    int w, h, comp;

    s->state = IMG_FAILED;
    if (!stbi_info_from_memory(data, len, &w, &h, &comp) || w <= 0 || h <= 0 ||
        (uint32_t)w * (uint32_t)h > MAX_SOURCE_PIXELS)
        return false;

    /* Shown size: the attributes', else 3/4 of the CSS pixels, never wider
     * than the screen. */
    int tw = s->want_w, th = s->want_h;
    if (tw && !th)
        th = (int)((int64_t)h * tw / w);
    else if (th && !tw)
        tw = (int)((int64_t)w * th / h);
    else if (!tw)
        tw = CSS_TO_DEV(w), th = CSS_TO_DEV(h);
    if (tw > MAX_W) {
        th = (int)((int64_t)th * MAX_W / tw);
        tw = MAX_W;
    }
    if (tw > w || th > h)
        tw = w > MAX_W ? MAX_W : w, th = (int)((int64_t)h * tw / w);    /* never upscale */
    if (tw < 1)
        tw = 1;
    if (th < 1)
        th = 1;
    if (th > 4 * 320)
        th = 4 * 320;

    uint32_t need = (uint32_t)tw * (uint32_t)th * 3;
    if (tree_image_bytes(page_root(p)) + need > IMAGE_BUDGET)
        return false;
    uint8_t *rgba = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
    if (!rgba)
        return false;
    uint8_t *mem = malloc(need);
    if (!mem) {
        stbi_image_free(rgba);
        return false;
    }
    uint16_t *px = (uint16_t *)mem;
    uint8_t *alpha = mem + (size_t)tw * (size_t)th * 2;
    bool any_alpha = false;

    /* Box filter: average the source block under each target pixel,
     * weighting colour by alpha so edges don't darken. */
    for (int y = 0; y < th; y++) {
        int sy0 = (int)((int64_t)y * h / th), sy1 = (int)((int64_t)(y + 1) * h / th);
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (int x = 0; x < tw; x++) {
            int sx0 = (int)((int64_t)x * w / tw), sx1 = (int)((int64_t)(x + 1) * w / tw);
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const uint8_t *q = rgba + ((size_t)yy * (size_t)w + (size_t)sx0) * 4;
                for (int xx = sx0; xx < sx1; xx++, q += 4) {
                    r += q[0] * q[3];
                    g += q[1] * q[3];
                    b += q[2] * q[3];
                    a += q[3];
                    n++;
                }
            }
            uint32_t ra = a ? r / a : 0, ga = a ? g / a : 0, ba = a ? b / a : 0;
            uint32_t av = a / n;
            px[y * tw + x] = (uint16_t)((ra >> 3) << 11 | (ga >> 2) << 5 | (ba >> 3));
            alpha[y * tw + x] = (uint8_t)av;
            any_alpha |= av < 255;
        }
    }
    stbi_image_free(rgba);

    s->mem = mem;
    s->w = tw;
    s->h = th;
    s->img.w = tw;
    s->img.h = th;
    s->img.px = px;
    s->img.alpha = any_alpha ? alpha : NULL;
    s->state = IMG_OK;
    p->img_bytes += need;
    return true;
}

void image_release(struct image_slot *s)
{
    free(s->mem);
    s->mem = NULL;
    if (s->state == IMG_OK)
        s->state = IMG_NONE;
}

uint8_t *web_inflate(const uint8_t *in, int len, bool zlib_header, int *out_len)
{
    return (uint8_t *)stbi_zlib_decode_malloc_guesssize_headerflag((const char *)in, len, len * 4 + 1024, out_len,
                                                                   zlib_header);
}
