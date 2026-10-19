/* The device passcode (see glaze/passcode.h). */

#include <glaze/notify.h>
#include <glaze/passcode.h>
#include <glaze/prefs.h>
#include <glaze/sys.h>
#include <string.h>

#define ROUNDS  4096

struct stored {
    uint32_t salt, hash;
};

static uint32_t fnv(uint32_t h, const void *p, uint32_t n)
{
    const uint8_t *b = p;

    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

static uint32_t hash(uint32_t salt, const char *code)
{
    uint32_t len = (uint32_t)strlen(code), h = 2166136261u;

    for (int i = 0; i < ROUNDS; i++) {
        h = fnv(h, &salt, sizeof(salt));
        h = fnv(h, code, len);
    }
    return h;
}

static bool load(struct stored *s)
{
    return pref_get(PASSCODE_PREF, s, sizeof(*s)) == (int)sizeof(*s);
}

bool passcode_enabled(void)
{
    struct stored s;

    return load(&s);
}

bool passcode_check(const char *code)
{
    struct stored s;

    return load(&s) && hash(s.salt, code) == s.hash;
}

int passcode_set(const char *code)
{
    struct gz_datetime dt;
    struct stored s;

    if (!code) {
        int rv = pref_delete(PASSCODE_PREF);
        return rv == GZ_ENOENT ? 0 : rv;
    }
    s.salt = gz_time_ms() * 2654435761u;
    if (!gz_rtc_get(&dt))
        s.salt ^= gz_dt_to_secs(&dt);
    s.hash = hash(s.salt, code);
    return pref_put(PASSCODE_PREF, &s, sizeof(s));
}
