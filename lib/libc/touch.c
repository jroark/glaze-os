#include <glaze/touch.h>

void touch_identity(struct touch_calibration *c)
{
    *c = (struct touch_calibration){ TOUCH_VERSION, { TOUCH_ONE, 0, 0 }, { 0, TOUCH_ONE, 0 } };
}

bool touch_valid(const struct touch_calibration *c)
{
    if (c->version != TOUCH_VERSION)
        return false;
    for (int i = 0; i < 3; i++) {
        int limit = (i == 2 ? 1024 : 4) * TOUCH_ONE;
        if (c->x[i] < -limit || c->x[i] > limit || c->y[i] < -limit || c->y[i] > limit)
            return false;
    }
    /* The fixed portrait panel must keep its orientation and usable scale. */
    int64_t det = (int64_t)c->x[0] * c->y[1] - (int64_t)c->x[1] * c->y[0];
    return c->x[0] > TOUCH_ONE / 4 && c->y[1] > TOUCH_ONE / 4 &&
           det >= (int64_t)TOUCH_ONE * TOUCH_ONE / 16;
}

static int rounded(int64_t v)
{
    return (int)((v + (v < 0 ? -TOUCH_ONE / 2 : TOUCH_ONE / 2)) / TOUCH_ONE);
}

void touch_apply(const struct touch_calibration *c, int x, int y, struct touch_point *out)
{
    out->x = rounded((int64_t)c->x[0] * x + (int64_t)c->x[1] * y + c->x[2]);
    out->y = rounded((int64_t)c->y[0] * x + (int64_t)c->y[1] * y + c->y[2]);
}

/* Divide to Q16 without overflowing when the covariance determinant is large.
 * The caller bounds coordinates so doubling a remainder always fits int64. */
static int32_t ratio(int64_t n, int64_t d)
{
    bool neg = n < 0;
    if (neg)
        n = -n;
    int64_t q = n / d, r = n % d;
    if (q > 4)
        return 5 * TOUCH_ONE;   /* rejected by touch_valid */
    for (int i = 0; i < 16; i++) {
        r *= 2;
        q = q * 2 + r / d;
        r %= d;
    }
    if (r * 2 >= d)
        q++;
    return (int32_t)(neg ? -q : q);
}

int touch_fit(struct touch_calibration *c, const struct touch_point raw[TOUCH_POINTS],
              const struct touch_point target[TOUCH_POINTS])
{
    int64_t sx = 0, sy = 0, xx = 0, xy = 0, yy = 0;
    struct touch_calibration fit = { .version = TOUCH_VERSION };
    for (int i = 0; i < TOUCH_POINTS; i++) {
        int x = raw[i].x, y = raw[i].y;
        if (x < -1024 || x > 2048 || y < -1024 || y > 2048 ||
            target[i].x < 0 || target[i].x >= GZ_FB_WIDTH ||
            target[i].y < 0 || target[i].y >= GZ_FB_HEIGHT)
            return GZ_EINVAL;
        sx += x; sy += y;
        xx += x * x; xy += x * y; yy += y * y;
    }
    xx = TOUCH_POINTS * xx - sx * sx;
    xy = TOUCH_POINTS * xy - sx * sy;
    yy = TOUCH_POINTS * yy - sy * sy;
    int64_t det = xx * yy - xy * xy;
    if (xx <= 0 || yy <= 0 || det <= 0)
        return GZ_EINVAL;
    for (int axis = 0; axis < 2; axis++) {
        int64_t st = 0, xt = 0, yt = 0;
        int32_t *v = axis ? fit.y : fit.x;
        for (int i = 0; i < TOUCH_POINTS; i++) {
            int t = axis ? target[i].y : target[i].x;
            st += t; xt += (int64_t)raw[i].x * t; yt += (int64_t)raw[i].y * t;
        }
        xt = TOUCH_POINTS * xt - sx * st;
        yt = TOUCH_POINTS * yt - sy * st;
        v[0] = ratio(xt * yy - yt * xy, det);
        v[1] = ratio(yt * xx - xt * xy, det);
        v[2] = (int32_t)((st * TOUCH_ONE - v[0] * sx - v[1] * sy) / TOUCH_POINTS);
    }
    if (!touch_valid(&fit))
        return GZ_EINVAL;
    for (int i = 0; i < TOUCH_POINTS; i++) {
        struct touch_point p;
        touch_apply(&fit, raw[i].x, raw[i].y, &p);
        int64_t dx = p.x - target[i].x, dy = p.y - target[i].y;
        if (dx * dx + dy * dy > 12 * 12)
            return GZ_EINVAL;
    }
    *c = fit;
    return 0;
}
