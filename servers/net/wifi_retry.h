/* Retry transient link loss without retrying rejected credentials or an
 * explicit disconnect. Keep the backoff while association is in progress. */
#ifndef GLAZE_WIFI_RETRY_H
#define GLAZE_WIFI_RETRY_H
#include <stdbool.h>
#include <stdint.h>

#define WIFI_RETRY_MIN_MS 2000u
#define WIFI_RETRY_MAX_MS 30000u
struct wifi_retry {
    uint32_t at, delay;
    bool scheduled;
};
static inline void wifi_retry_reset(struct wifi_retry *r)
{
    *r = (struct wifi_retry){.delay = WIFI_RETRY_MIN_MS};
}
static inline void wifi_retry_poll(struct wifi_retry *r, uint32_t now,
                                   bool enabled, bool connected, bool connecting,
                                   bool auth_failed, void (*connect)(void *), void *ctx)
{
    if (!enabled || connected || auth_failed) {
        wifi_retry_reset(r);
        return;
    }
    if (!r->scheduled) {
        r->at = now + r->delay;
        r->scheduled = true;
    }
    if (connecting || (int32_t)(now - r->at) < 0)
        return;
    r->delay = r->delay < WIFI_RETRY_MAX_MS / 2 ? r->delay * 2 : WIFI_RETRY_MAX_MS;
    r->at = now + r->delay;
    connect(ctx);
}
#endif
