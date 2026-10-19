/*
 * libgfx: software RGB565 rendering.
 *
 * Colours passed to drawing calls are 24-bit 0xRRGGBB; surfaces store
 * RGB565. Gradients are ordered-dithered to hide 565 banding. Alpha values
 * are 0..255.
 */
#ifndef GLAZE_GFX_H
#define GLAZE_GFX_H

#include <glaze/types.h>

typedef struct {
    int x, y, w, h;
} gfx_rect;

typedef struct {
    uint16_t *px;
    int w, h;
    int stride;             /* pixels per row */
    gfx_rect clip;
} gfx_surface;

/* GZI image: "GZI1" u16 w, u16 h, u16 flags, u16 pad, w*h RGB565,
 * then w*h A8 when flags & GZI_ALPHA. Colour is not premultiplied. */
#define GZI_ALPHA   1
typedef struct {
    int w, h;
    const uint16_t *px;
    const uint8_t *alpha;   /* NULL for opaque images */
} gfx_image;

/* GZF font: "GZF1" u16 size, u16 line_height, i16 ascent, i16 descent,
 * u16 first, u16 count, then `count` glyph records, then A8 bitmaps. */
struct gfx_glyph {
    uint16_t adv16;         /* advance in 1/16 px */
    uint8_t w, h;
    int8_t left;            /* bitmap x offset from the pen */
    int8_t top;             /* bitmap top, pixels above the baseline */
    uint16_t pad;
    uint32_t off;           /* into the bitmap area */
};

typedef struct {
    int size, line_height, ascent, descent;
    int first, count;
    const struct gfx_glyph *glyphs;
    const uint8_t *bitmap;
} gfx_font;

/* Extra glyphs past ASCII, usable in strings. Fonts also cover Latin-1
 * (0xA0-0xFF) at its own code points; 0x88-0x9F render as '?'. */
#define GFX_CH_TIMES    "\x7f"
#define GFX_CH_DIVIDE   "\x80"
#define GFX_CH_MINUS    "\x81"
#define GFX_CH_DEGREE   "\x82"
#define GFX_CH_BULLET   "\x83"
#define GFX_CH_ELLIPSIS "\x84"
#define GFX_CH_LARROW   "\x85"
#define GFX_CH_RARROW   "\x86"
#define GFX_CH_CHECK    "\x87"

#define RGB565(rgb) ((uint16_t)((((rgb) >> 8) & 0xF800) | (((rgb) >> 5) & 0x07E0) | (((rgb) >> 3) & 0x001F)))

static inline gfx_rect gfx_r(int x, int y, int w, int h)
{
    gfx_rect r = { x, y, w, h };
    return r;
}

bool gfx_rect_intersect(gfx_rect a, gfx_rect b, gfx_rect *out);
gfx_rect gfx_rect_union(gfx_rect a, gfx_rect b);
static inline bool gfx_rect_empty(gfx_rect r) { return r.w <= 0 || r.h <= 0; }
static inline bool gfx_rect_contains(gfx_rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void gfx_surface_init(gfx_surface *s, uint16_t *px, int w, int h, int stride);
void gfx_set_clip(gfx_surface *s, gfx_rect r);     /* intersected with bounds */
void gfx_reset_clip(gfx_surface *s);

uint16_t gfx_blend565(uint16_t dst, uint16_t src, int alpha);

/* Fills. */
void gfx_fill(gfx_surface *s, gfx_rect r, uint32_t rgb);
void gfx_fill_alpha(gfx_surface *s, gfx_rect r, uint32_t rgb, int alpha);
void gfx_vgradient(gfx_surface *s, gfx_rect r, uint32_t top, uint32_t bottom, int alpha);
void gfx_hgradient(gfx_surface *s, gfx_rect r, uint32_t left, uint32_t right, int alpha);
void gfx_round_rect(gfx_surface *s, gfx_rect r, int radius, uint32_t rgb, int alpha);
void gfx_round_rect_vgradient(gfx_surface *s, gfx_rect r, int radius, uint32_t top, uint32_t bottom, int alpha);
void gfx_circle(gfx_surface *s, int cx, int cy, int radius, uint32_t rgb, int alpha);
void gfx_line(gfx_surface *s, int x0, int y0, int x1, int y1, uint32_t rgb, int alpha);
/* Anti-aliased thick line between fixed-point (1/16 px) endpoints. */
void gfx_line_aa(gfx_surface *s, int x0_16, int y0_16, int x1_16, int y1_16, int width_16, uint32_t rgb, int alpha);

/* Blits. */
void gfx_blit(gfx_surface *dst, int x, int y, const gfx_surface *src, gfx_rect sr);
void gfx_blit_alpha(gfx_surface *dst, int x, int y, const gfx_surface *src, gfx_rect sr, int alpha);
void gfx_blit_scaled(gfx_surface *dst, gfx_rect dr, const gfx_surface *src, gfx_rect sr, int alpha);
void gfx_draw_image(gfx_surface *dst, int x, int y, const gfx_image *img, int alpha);
void gfx_draw_image_scaled(gfx_surface *dst, gfx_rect dr, const gfx_image *img, int alpha);
/* Tint an alpha-only use of `img` (its A8 plane) with a solid colour. */
void gfx_draw_mask(gfx_surface *dst, int x, int y, const gfx_image *img, uint32_t rgb, int alpha);
/* Stretch an image horizontally: left/right caps kept, middle column repeated. */
void gfx_draw_3slice(gfx_surface *dst, gfx_rect dr, const gfx_image *img, int cap_l, int cap_r, int alpha);

/* Images and fonts are parsed in place from romfs-mapped data. */
bool gfx_image_parse(gfx_image *img, const void *data, size_t size);
bool gfx_font_parse(gfx_font *f, const void *data, size_t size);

/* Text. `y` is the top of the line box; returns the advance in pixels. */
int gfx_text(gfx_surface *s, int x, int y, const gfx_font *f, const char *str, uint32_t rgb, int alpha);
int gfx_text_n(gfx_surface *s, int x, int y, const gfx_font *f, const char *str, int n, uint32_t rgb, int alpha);
int gfx_text_width(const gfx_font *f, const char *str);
int gfx_text_width_n(const gfx_font *f, const char *str, int n);
/* Text with a 1px drop shadow (offset dy) as used on SpringBoard labels. */
int gfx_text_shadow(gfx_surface *s, int x, int y, const gfx_font *f, const char *str, uint32_t rgb,
                    uint32_t shadow_rgb, int dy, int alpha);
/* Horizontally centred within [x, x+w). */
void gfx_text_center(gfx_surface *s, int x, int y, int w, const gfx_font *f, const char *str, uint32_t rgb, int alpha);
/* Number of chars of `str` that fit in `max_w` pixels, with ellipsis if cut. */
void gfx_text_fit(gfx_surface *s, int x, int y, int max_w, const gfx_font *f, const char *str, uint32_t rgb, int alpha);

#endif
