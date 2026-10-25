/* System-wide display adjustments. */
#ifndef GLAZE_DISPLAY_H
#define GLAZE_DISPLAY_H
#include <glaze/abi.h>

#define DISPLAY_PREF "sb.display"
#define DISPLAY_VERSION 1u
/* Native levels from the stock BE-300 ddi.dll. Brightness 0 duplicates
 * level 1, so expose the five distinct levels. Contrast has ten levels. */
#define DISPLAY_BRIGHTNESS_MIN 1
#define DISPLAY_BRIGHTNESS_MAX 5
#define DISPLAY_BRIGHTNESS_DEFAULT 3
#define DISPLAY_CONTRAST_MAX 9
#define DISPLAY_CONTRAST_DEFAULT 5
#define DISPLAY_DEFAULTS { DISPLAY_VERSION, DISPLAY_BRIGHTNESS_DEFAULT, DISPLAY_CONTRAST_DEFAULT }

struct display_settings {
    uint32_t version;
    int32_t brightness, contrast;
};

static inline bool display_valid(const struct display_settings *v)
{
    return v->version == DISPLAY_VERSION && v->brightness >= DISPLAY_BRIGHTNESS_MIN &&
           v->brightness <= DISPLAY_BRIGHTNESS_MAX && v->contrast >= 0 && v->contrast <= DISPLAY_CONTRAST_MAX;
}

#endif
