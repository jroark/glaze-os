/* Portrait touch calibration, in factory-scaled full-screen coordinates. */
#ifndef GLAZE_TOUCH_H
#define GLAZE_TOUCH_H

#include <glaze/abi.h>

#define TOUCH_PREF "sb.touch"
#define TOUCH_VERSION 1u
#define TOUCH_ONE 65536
#define TOUCH_POINTS 5
#define TOUCH_PREVIEW_MS 30000u

struct touch_point { int32_t x, y; };
struct touch_calibration {
    uint32_t version;
    int32_t x[3], y[3];    /* Q16: screen x/y = a * input x + b * input y + c */
};

void touch_identity(struct touch_calibration *c);
bool touch_valid(const struct touch_calibration *c);
void touch_apply(const struct touch_calibration *c, int x, int y, struct touch_point *out);
/* Five-point least squares; rejects collinear, implausible or inaccurate fits. */
int touch_fit(struct touch_calibration *c, const struct touch_point raw[TOUCH_POINTS],
              const struct touch_point target[TOUCH_POINTS]);

#endif
