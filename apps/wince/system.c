/* The stock settings applets' system services, bridged to Glaze's own:
 * the device password (Glaze's passcode), wave volume and click sounds
 * (Glaze's sound preferences), touch calibration (SpringBoard's) and
 * CE user notifications (SpringBoard's timed notifications).
 * Password APIs: https://learn.microsoft.com/en-us/previous-versions/windows/embedded/ms908052(v=msdn.10)
 * CeSetUserNotification: https://learn.microsoft.com/en-us/previous-versions/windows/embedded/aa922123(v=msdn.10) */
#include "runtime.h"
#include <glaze/audio.h>
#include <glaze/notify.h>
#include <glaze/passcode.h>
#include <glaze/prefs.h>
#include <glaze/sb.h>
#include <glaze/touch.h>

/* ---- password ----------------------------------------------------------- */

/* Glaze's passcode is four digits; a CE password that is not cannot become
 * it, and the applet is told so rather than storing something else. */
static bool passcode_text(const WCHAR *w, char out[5])
{
    unsigned n = 0;
    for (; w && w[n]; n++)
        if (n >= 4 || w[n] < '0' || w[n] > '9')
            return false;
    for (unsigned i = 0; i < n; i++)
        out[i] = (char)w[i];
    out[n] = 0;
    return n == 0 || n == 4;
}
static void passcode_refused(void)
{
    static const WCHAR text[] = u"Glaze's passcode is four digits (0-9). Choose a four-digit password to use it here.";
    static const WCHAR title[] = u"Password";
    CW ce_MessageBoxW(CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW);
    ce_MessageBoxW(0, WORD(text), WORD(title), 0x30, 0, 0, 0, 0, 0, 0, 0, 0);
}
CE_API(GetPasswordActive)
{
    return passcode_enabled();
}
/* CheckPassword(text): an empty password matches when none is set. */
CE_API(CheckPassword)
{
    char code[5];
    if (!passcode_enabled())
        return !a0 || !*PTR(WCHAR, a0);
    return passcode_text(PTR(WCHAR, a0), code) && code[0] && passcode_check(code);
}
/* SetPassword(old, new): the old one must match; an empty new one turns the
 * passcode off. */
CE_API(SetPassword)
{
    char old[5], code[5];
    if (passcode_enabled() && !(passcode_text(PTR(WCHAR, a0), old) && passcode_check(old))) {
        ce_error = 86; /* ERROR_INVALID_PASSWORD */
        return 0;
    }
    if (!passcode_text(PTR(WCHAR, a1), code)) {
        passcode_refused();
        ce_error = 86;
        return 0;
    }
    return passcode_set(code[0] ? code : NULL) >= 0;
}
/* SetPasswordActive(active, password): Glaze has no inactive passcode, so
 * deactivating removes it and activating needs the code itself. */
CE_API(SetPasswordActive)
{
    char code[5];
    if (!passcode_text(PTR(WCHAR, a1), code)) {
        passcode_refused();
        ce_error = 86;
        return 0;
    }
    if (!a0)
        return !passcode_enabled() || (code[0] && passcode_check(code) && passcode_set(NULL) >= 0);
    if (passcode_enabled())
        return code[0] && passcode_check(code);
    return code[0] && passcode_set(code) >= 0;
}

/* ---- sound -------------------------------------------------------------- */

/* Wave volume is two 16-bit channels; Glaze's is 0-10 for both. */
CE_API(waveOutGetVolume)
{
    int32_t level = AUDIO_VOLUME_DEFAULT;
    if (!a1) {
        ce_error = 87;
        return 11; /* MMSYSERR_INVALPARAM */
    }
    pref_get(AUDIO_PREF_VOLUME, &level, sizeof(level));
    CW v = (CW)MAX(0, MIN(AUDIO_VOLUME_MAX, level)) * 0xffffu / AUDIO_VOLUME_MAX;
    *PTR(CW, a1) = v | v << 16;
    return 0;
}
static void set_volume(CW value)
{
    int32_t level = (int32_t)(((value & 0xffffu) * AUDIO_VOLUME_MAX + 0x7fffu) / 0xffffu);
    pref_put(AUDIO_PREF_VOLUME, &level, sizeof(level));
}
CE_API(waveOutSetVolume)
{
    set_volume(a1);
    audio_sound_reload();
    return 0;
}
/* The Sound applet saves HKCU\ControlPanel\Volume first: Volume (a wave
 * volume), BuzzScreen (taps) and BuzzKey (key clicks). */
CE_API(AudioUpdateFromRegistry)
{
    const char *key = "HKCU\\ControlPanel\\Volume";
    set_volume(ce_registry_dword(key, "Volume", 0xffffffffu));
    int32_t taps = ce_registry_dword(key, "BuzzScreen", 0) != 0, keys = ce_registry_dword(key, "BuzzKey", 1) != 0;
    pref_put(AUDIO_PREF_TOUCH, &taps, sizeof(taps));
    pref_put(AUDIO_PREF_KEYS, &keys, sizeof(keys));
    audio_sound_reload();
    return 0;
}

/* ---- touch calibration -------------------------------------------------- */

static int touch_request(uint32_t op, const struct touch_calibration *c)
{
    struct gz_msg m = {{0}}, r = {{0}};
    m.w[0] = op;
    m.w[3] = (uint32_t)gz_getpid();
    if (c)
        memcpy(&m.w[4], c, sizeof(*c));
    int rv = gz_send(ui.sb_port, &m, &r);
    return rv < 0 ? rv : (int)r.w[0];
}
/* SpringBoard's full-screen targets, as Settings uses them (glaze/touch.h). */
static const struct touch_point targets[TOUCH_POINTS] = {{120, 168}, {24, 40}, {216, 40}, {216, 296}, {24, 296}};
static void touch_screen(int step, const char *message)
{
    gfx_fill(&ui.surf, gfx_r(0, 0, 240, 320), 0xffffff);
    gfx_text_center(&ui.surf, 0, 110, 240, ui_font(FONT_B14), "Align Screen", 0x222222, 255);
    gfx_text_center(&ui.surf, 0, 140, 240, ui_font(FONT_R12), message, 0x444444, 255);
    gfx_text_center(&ui.surf, 0, 200, 240, ui_font(FONT_R11), "Home or Esc cancels.", 0x666666, 255);
    if (step >= 0 && step < TOUCH_POINTS) {
        int x = targets[step].x, y = targets[step].y;
        gfx_fill(&ui.surf, gfx_r(x - 10, y, 21, 1), 0xc00000);
        gfx_fill(&ui.surf, gfx_r(x, y - 10, 1, 21), 0xc00000);
    }
    ui_invalidate_all();
    ui_present();
}
/* TouchCalibrate(): Glaze's five-point calibration (as in Settings), on the
 * full screen this runtime owns; the result is saved like Settings saves it.
 */
CE_API(TouchCalibrate)
{
    struct touch_point samples[TOUCH_POINTS], window[3] = {{0, 0}}, origin = {0, 0};
    if (touch_request(SB_TOUCH_BEGIN, NULL) < 0)
        return 0;
    int step = 0, count = 0;
    bool down = false, moved = false;
    const char *message = "Tap the center of the cross.";
    touch_screen(step, message);
    for (;;) {
        struct gz_input_event e;
        if (!ui_next_event(&e, 1000))
            continue;
        if (e.type == GZ_EV_TOUCH_CAL_END || e.type == GZ_EV_APP_DEACTIVATE ||
            (e.type == GZ_EV_KEY_DOWN && e.code == GZ_KEY_ESC)) {
            touch_request(SB_TOUCH_CANCEL, NULL);
            ce_redraw_all();
            return 0;
        }
        if (e.type != GZ_EV_TOUCH_DOWN && e.type != GZ_EV_TOUCH_MOVE && e.type != GZ_EV_TOUCH_UP)
            continue;
        struct touch_point p = {e.x, e.y};
        if (e.type == GZ_EV_TOUCH_DOWN) {
            down = true;
            moved = false;
            count = 0;
            origin = p;
        }
        if (!down)
            continue;
        int64_t dx = p.x - origin.x, dy = p.y - origin.y;
        if (dx * dx + dy * dy > 12 * 12)
            moved = true;
        if (e.type != GZ_EV_TOUCH_UP) {
            window[count++ % 3] = p;
            continue;
        }
        down = false;
        if (moved || !count) {
            touch_screen(step, "Hold steady and tap again.");
            continue;
        }
        struct touch_point sum = {0, 0};
        int n = MIN(3, count);
        for (int i = 0; i < n; i++) {
            sum.x += window[i].x;
            sum.y += window[i].y;
        }
        samples[step++] = (struct touch_point){sum.x / n, sum.y / n};
        if (step < TOUCH_POINTS) {
            touch_screen(step, message);
            continue;
        }
        struct touch_calibration c;
        if (touch_fit(&c, samples, targets) < 0 || touch_request(SB_TOUCH_PREVIEW, &c) < 0 ||
            touch_request(SB_TOUCH_SAVE, NULL) < 0) {
            touch_request(SB_TOUCH_CANCEL, NULL);
            step = 0;
            if (touch_request(SB_TOUCH_BEGIN, NULL) < 0)
                break;
            touch_screen(step, "Targets did not match. Try again.");
            continue;
        }
        ce_redraw_all();
        return 1;
    }
    ce_redraw_all();
    return 0;
}

/* ---- user notifications ------------------------------------------------- */

/* CeSetUserNotification(handle, application, SYSTEMTIME, CE_USER_NOTIFICATION
 * {flags, title, text, sound, ...}) posts a Glaze notification for this CE
 * context at that local time. Handles are kept in the shared registry
 * (HKLM\Software\Glaze\CeNotify\<handle>), since an alarm outlives the app
 * that set it, and the next one in its Next value. */
#define NOTIFY_KEY "HKLM\\Software\\Glaze\\CeNotify"
CE_API(CeSetUserNotification)
{
    const uint16_t *st = PTR(uint16_t, a2);
    const uint8_t *note = PTR(uint8_t, a3);
    if (!st) {
        ce_error = 87;
        return 0;
    }
    struct gz_datetime dt = {.year = st[0], .month = (uint8_t)st[1], .day = (uint8_t)st[3],
                             .hour = (uint8_t)st[4], .minute = (uint8_t)st[5], .second = (uint8_t)st[6]};
    CW handle = a0;
    if (!handle) {
        handle = ce_registry_dword(NOTIFY_KEY, "Next", 1);
        ce_registry_set_dword(NOTIFY_KEY, "Next", handle + 1);
    }
    char title[32] = "Alarm", body[96] = "", name[16];
    if (note && ce_u32(note + 4))
        ce_display(title, sizeof(title), PTR(WCHAR, ce_u32(note + 4)));
    if (note && ce_u32(note + 8))
        ce_display(body, sizeof(body), PTR(WCHAR, ce_u32(note + 8)));
    if (!body[0] && a1)
        ce_display(body, sizeof(body), PTR(WCHAR, a1));
    snprintf(name, sizeof(name), "%u", (unsigned)handle);
    ce_registry_set_dword(NOTIFY_KEY, name, gz_dt_to_secs(&dt));
    int rv = sb_note_post_for(res_bundle(), handle, gz_dt_to_secs(&dt), title, body, 0);
    printf("wince: notification %u at %04u-%02u-%02u %02u:%02u (%d)\n", (unsigned)handle, dt.year, dt.month,
           dt.day, dt.hour, dt.minute, rv);
    return handle;
}
CE_API(CeClearUserNotification)
{
    char name[16];
    snprintf(name, sizeof(name), "%u", (unsigned)a0);
    if (!a0 || !ce_registry_dword(NOTIFY_KEY, name, 0)) {
        ce_error = 87;
        return 0;
    }
    ce_registry_set_dword(NOTIFY_KEY, name, 0);
    sb_note_cancel_for(res_bundle(), a0);
    return 1;
}
/* Details of a notification are not kept; callers see an unknown handle. */
CE_API(CeGetUserNotification)
{
    if (a2)
        *PTR(CW, a2) = 0;
    ce_error = 87;
    return 0;
}

/* GetDisk's CGD*UserNotification wrap the CE calls with a database mirror,
 * the alarm backup Glaze's notifications make unnecessary. */
void *ce_resolve_system(const char *dll, const char *name)
{
    if (!strcasecmp(dll, "GetDisk.dll")) {
        if (!strcmp(name, "CGDSetUserNotification"))
            return (void *)ce_CeSetUserNotification;
        if (!strcmp(name, "CGDClearUserNotification"))
            return (void *)ce_CeClearUserNotification;
        if (!strcmp(name, "CGDGetUserNotification"))
            return (void *)ce_CeGetUserNotification;
        return NULL;
    }
    if (strcasecmp(dll, "coredll.dll"))
        return NULL;
    static const struct ce_export exports[] = {
        CE_EXPORT(GetPasswordActive), CE_EXPORT(CheckPassword), CE_EXPORT(SetPassword),
        CE_EXPORT(SetPasswordActive), CE_EXPORT(waveOutGetVolume), CE_EXPORT(waveOutSetVolume),
        CE_EXPORT(AudioUpdateFromRegistry), CE_EXPORT(TouchCalibrate), CE_EXPORT(CeSetUserNotification),
        CE_EXPORT(CeClearUserNotification), CE_EXPORT(CeGetUserNotification),
    };
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
