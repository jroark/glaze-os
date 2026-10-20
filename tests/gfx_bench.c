/* Unsanitized renderer timings. GFX_BASELINE optionally links a renamed
 * snapshot of the old renderer, compiled with the same flags. */
#include <glaze/gfx.h>
#include <stdio.h>
#ifdef __mips__
#include <glaze/sys.h>
static uint64_t now_us(void)
{
    return gz_time_us();
}
#define REPEATS 20
#else
#include <time.h>
static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
#define REPEATS 2000
#endif

struct renderer {
    void (*solid)(gfx_surface *, gfx_rect, int, uint32_t, int);
    void (*roundgrad)(gfx_surface *, gfx_rect, int, uint32_t, uint32_t, int);
    void (*gradient)(gfx_surface *, gfx_rect, uint32_t, uint32_t, int);
    void (*slice)(gfx_surface *, gfx_rect, const gfx_image *, int, int, int);
};
#ifdef GFX_BASELINE
void before_gfx_round_rect(gfx_surface *, gfx_rect, int, uint32_t, int);
void before_gfx_round_rect_vgradient(gfx_surface *, gfx_rect, int, uint32_t, uint32_t, int);
void before_gfx_vgradient(gfx_surface *, gfx_rect, uint32_t, uint32_t, int);
void before_gfx_draw_3slice(gfx_surface *, gfx_rect, const gfx_image *, int, int, int);
static const struct renderer baseline = {before_gfx_round_rect, before_gfx_round_rect_vgradient,
                                         before_gfx_vgradient, before_gfx_draw_3slice};
#endif
static const struct renderer current = {gfx_round_rect, gfx_round_rect_vgradient, gfx_vgradient,
                                        gfx_draw_3slice};
static uint16_t pixels[240 * 304], image[30 * 24];
static uint8_t mask[30 * 24];
static const char *names[] = {"solid cell",  "selected cell", "nav gradient",
                              "back button", "bar button",    "alpha overlay"};

static void draw(const struct renderer *f, gfx_surface *s, int which)
{
    gfx_rect r = gfx_r(10, 42, 220, 30);
    gfx_image img = {30, 24, image, mask};
    switch (which) {
    case 0:
        f->solid(s, r, 8, 0xffffff, 255);
        break;
    case 1:
        f->solid(s, r, 8, 0xa9abae, 255);
        f->roundgrad(s, gfx_r(11, 43, 218, 28), 7, 0x058cf5, 0x015ee6, 255);
        break;
    case 2:
        f->gradient(s, gfx_r(0, 0, 240, 30), 0xb4c0d0, 0x6d84a2, 255);
        break;
    case 3:
        f->slice(s, gfx_r(5, 3, 80, 24), &img, 12, 6, 255);
        break;
    case 4:
        f->solid(s, gfx_r(5, 3, 70, 24), 5, 0x2a3442, 220);
        f->roundgrad(s, gfx_r(6, 4, 68, 22), 4, 0x7e93af, 0x4a6383, 255);
        f->solid(s, gfx_r(6, 4, 68, 11), 4, 0xffffff, 30);
        break;
    case 5:
        f->roundgrad(s, gfx_r(10, 30, 220, 70), 9, 0x2f4a7d, 0x0e2550, 235);
        break;
    }
}

int main(void)
{
    gfx_surface s;
    gfx_surface_init(&s, pixels, 240, 304, 240);
    for (int i = 0; i < 30 * 24; i++) {
        image[i] = (uint16_t)(i * 39);
        mask[i] = i % 7 ? 255 : 128;
    }
    for (int sample = 0; sample < 3; sample++) {
        for (int which = 0; which < 6; which++) {
#ifdef GFX_BASELINE
            uint64_t start = now_us();
            for (int i = 0; i < REPEATS; i++)
                draw(&baseline, &s, which);
            printf("gfx-bench: %s before %u us/%u draws\n", names[which], (unsigned)(now_us() - start),
                   REPEATS);
#endif
            uint64_t start_after = now_us();
            for (int i = 0; i < REPEATS; i++)
                draw(&current, &s, which);
            printf("gfx-bench: %s after %u us/%u draws\n", names[which], (unsigned)(now_us() - start_after),
                   REPEATS);
        }
    }
    return 0;
}
