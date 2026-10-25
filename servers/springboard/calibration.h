#ifndef SB_CALIBRATION_H
#define SB_CALIBRATION_H
#include <glaze/touch.h>

/* SpringBoard owns the session and restores the saved mapping independently
 * of Settings' event loop or lifetime. Zero owner means no active session. */
struct sb_calibration {
    struct touch_calibration saved, candidate;
    int owner;
    bool preview;
    uint32_t until;
};

void sb_calibration_load(struct sb_calibration *s);
int sb_calibration_request(struct sb_calibration *s, uint32_t op, int pid,
                           const struct touch_calibration *candidate, uint32_t now);
bool sb_calibration_expire(struct sb_calibration *s, uint32_t now);
void sb_calibration_cancel(struct sb_calibration *s);
/* Returns true when this touch must go directly to the sampling app. */
bool sb_calibration_input(const struct sb_calibration *s, struct gz_input_event *ev);
#endif
