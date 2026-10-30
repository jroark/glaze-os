/* System sound counterparts for the separate monophonic piezo. PCM/WAV,
 * music and application streams retain their headphone path. */
#include "buzzer.h"
#include <glaze/audio.h>
#include <glaze/sys.h>

struct note { uint16_t hz, ms; };
static const struct note tones[AUDIO_TONES][6] = {
    {{880,90},{0,40},{1175,90},{0,40},{1568,150}},
    {{1047,160},{0,60},{1568,220}},
    {{1760,280}},
    {{660,80},{0,40},{880,100}},
    {{1800,160},{0,120},{1800,160}},
};
static struct note click;
static const struct note *pattern;
static unsigned index, pulse;
static uint32_t due, started;
static bool alert, repeating, waiting;
bool buzzer_busy(void) { return pattern != NULL; }

static void stop(void)
{
    gz_buzzer_tone(0,0,0);
    pattern = NULL;
    alert = repeating = waiting = false;
}
static void drive(uint32_t now)
{
    const struct note *n = &pattern[index];
    due = now + n->ms;
    if (n->hz) gz_buzzer_tone(n->hz,n->ms,pulse);
    else gz_buzzer_tone(0,0,0);
}
void buzzer_volume(int volume)
{
    pulse = (unsigned)MAX(0,MIN(volume,AUDIO_VOLUME_MAX)) * 5;
    if (!pulse) stop();
    else if (pattern && !waiting && pattern[index].hz) {
        uint32_t now = gz_time_ms();
        if ((int32_t)(due-now)>0) gz_buzzer_tone(pattern[index].hz,due-now,pulse);
    }
}
void buzzer_click(unsigned sound)
{
    if (!pulse || (pattern && alert)) return;
    static const struct note clicks[]={{1800,10},{2200,12},{1600,25},{700,40},{1000,40}};
    if (sound >= ARRAY_SIZE(clicks)) return;
    click = clicks[sound];
    pattern = &click; index = 0; alert = repeating = waiting = false;
    started = gz_time_ms(); drive(started);
}
void buzzer_alert(int tone, bool repeat)
{
    if (!pulse || tone < 0 || tone >= AUDIO_TONES) return;
    pattern = tones[tone]; index = 0; alert = true; repeating = repeat; waiting = false;
    started = gz_time_ms(); drive(started);
}
void buzzer_stop_alert(void) { if (alert) stop(); }
int buzzer_service(void)
{
    if (!pattern) return -1;
    uint32_t now = gz_time_ms();
    if (repeating && now-started >= 60000) { stop(); return -1; }
    if ((int32_t)(now-due)<0) return (int)(due-now);
    if (waiting) { waiting = false; index = 0; drive(now); }
    else {
        if (!alert || ++index >= ARRAY_SIZE(tones[0]) || !pattern[index].ms) {
            if (!repeating) { stop(); return -1; }
            gz_buzzer_tone(0,0,0); waiting = true; due = now + 750;
        } else drive(now);
    }
    return (int)(due-now);
}
