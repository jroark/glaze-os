/* Sample-rate and channel conversion to a hardware rate (glaze/media.h). */

#include <glaze/media.h>
#include <string.h>

static const uint32_t hw_rates[] = { 8000, 11025, 16000, 22050, 32000, 44100 };

uint32_t media_hw_rate(uint32_t rate)
{
    uint32_t best = hw_rates[0];

    for (uint32_t i = 0; i < ARRAY_SIZE(hw_rates); i++) {
        uint32_t d = rate > hw_rates[i] ? rate - hw_rates[i] : hw_rates[i] - rate;
        uint32_t bd = rate > best ? rate - best : best - rate;
        if (d < bd)
            best = hw_rates[i];
    }
    return best;
}

void media_rs_init(struct media_resampler *rs, uint32_t in_rate, int in_ch, uint32_t out_rate, int out_ch)
{
    memset(rs, 0, sizeof(*rs));
    rs->in_rate = in_rate;
    rs->out_rate = out_rate;
    rs->in_ch = (uint16_t)in_ch;
    rs->out_ch = (uint16_t)out_ch;
    rs->step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
}

static void put(const struct media_resampler *rs, int16_t *o, int l, int r)
{
    if (rs->out_ch == 1) {
        o[0] = (int16_t)(rs->in_ch == 2 ? (l + r) >> 1 : l);
    } else {
        o[0] = (int16_t)l;
        o[1] = (int16_t)(rs->in_ch == 2 ? r : l);
    }
}

int media_rs_run(struct media_resampler *rs, const int16_t *in, int in_frames,
                 int16_t *out, int out_max, int *in_used)
{
    int ich = rs->in_ch, och = rs->out_ch;
    int used = 0, n = 0;

    if (rs->in_rate == rs->out_rate) {
        n = MIN(in_frames, out_max);
        for (int i = 0; i < n; i++)
            put(rs, out + i * och, in[i * ich], in[i * ich + ich - 1]);
        *in_used = n;
        return n;
    }
    if (!rs->primed && in_frames > 0) {
        rs->prev[0] = in[0];
        rs->prev[1] = in[ich - 1];
        rs->primed = true;
        used = 1;
    }
    /* Output sits `frac` of the way from prev to in[used]. */
    while (n < out_max && used < in_frames) {
        if (rs->frac >= 0x10000) {
            rs->frac -= 0x10000;
            rs->prev[0] = in[used * ich];
            rs->prev[1] = in[used * ich + ich - 1];
            used++;
            continue;
        }
        int f = (int)(rs->frac >> 1);     /* 15 bits keeps the product in range */
        int l = rs->prev[0] + (((in[used * ich] - rs->prev[0]) * f) >> 15);
        int r = rs->prev[1] + (((in[used * ich + ich - 1] - rs->prev[1]) * f) >> 15);
        put(rs, out + n * och, l, r);
        n++;
        rs->frac += rs->step;
    }
    *in_used = used;
    return n;
}
