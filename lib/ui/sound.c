/* libui: tap and swipe sounds for every app. A touch that lifts close to
 * where it went down is a tap; a quick, mostly horizontal one is a swipe.
 * The audio server decides from the snd.* settings whether either plays. */

#include "sound.h"
#include <glaze/audio.h>
#include <glaze/sys.h>

#define TAP_SLOP        8       /* px */
#define SWIPE_MIN       40      /* px */
#define SWIPE_MS        400

static int down_x, down_y;
static uint32_t down_ms;
static bool down, sounded;

void uisnd_sounded(void)
{
    sounded = true;
}

void uisnd_event(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_TOUCH_DOWN) {
        down = true;
        sounded = false;
        down_x = e->x;
        down_y = e->y;
        down_ms = gz_time_ms();
        return;
    }
    if (e->type != GZ_EV_TOUCH_UP || !down)
        return;
    down = false;
    if (sounded)
        return;
    int dx = e->x - down_x, dy = e->y - down_y;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;
    if (dx < TAP_SLOP && dy < TAP_SLOP)
        audio_sound(AUDIO_SND_TAP);
    else if (dx >= SWIPE_MIN && dx > 2 * dy && gz_time_ms() - down_ms < SWIPE_MS)
        audio_sound(AUDIO_SND_SWIPE);
}
