/*
 * Setting the clock by NTP (glaze/ntp.h) while Settings' "Set
 * Automatically" (TZ_PREF_AUTO) is on. A thread of its own does it, since
 * a query blocks for seconds: it syncs once the network is up, then every
 * SYNC_MS, backing off after failures (no card, no DHCP lease, no reply).
 * SB_NOTIFY_TIME from Settings wakes it to look at the preferences again.
 * It touches no SpringBoard state; after setting the clock it notifies the
 * main loop with SB_NOTIFY_CLOCK so the status bar is redrawn.
 */

#include "sb_internal.h"

#include <glaze/notify.h>
#include <glaze/ntp.h>
#include <glaze/prefs.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <glaze/tz.h>
#include <stdio.h>

#define SYNC_MS         (6 * 3600 * 1000)
#define RETRY_MIN_MS    (30 * 1000)
#define RETRY_MAX_MS    (30 * 60 * 1000)
#define RECENT_SECS     60      /* Settings has just synced: skip */

static int wake_sem = -1;
static int main_port;

static bool auto_on(void)
{
    int32_t on = 1;

    pref_get(TZ_PREF_AUTO, &on, sizeof(on));
    return on != 0;
}

static bool synced_recently(void)
{
    struct gz_datetime dt;
    uint32_t last;

    if (gz_rtc_get(&dt) || pref_get(TZ_PREF_SYNCED, &last, sizeof(last)) != sizeof(last))
        return false;
    return gz_dt_to_secs(&dt) - last < RECENT_SECS;
}

static void worker(void *arg)
{
    int retry = RETRY_MIN_MS, wait = 0;

    for (;;) {
        if (wait)
            gz_sem_wait(wake_sem, wait);
        while (gz_sem_wait(wake_sem, 0) == 0)
            ;
        if (!auto_on()) {
            wait = -1;                  /* until Settings turns it on */
            continue;
        }
        if (synced_recently()) {
            wait = SYNC_MS;
            continue;
        }
        int rv = ntp_sync();
        if (rv == 0) {
            retry = RETRY_MIN_MS;
            wait = SYNC_MS;
            gz_notify(main_port, SB_NOTIFY_CLOCK);
        } else {
            printf("springboard: ntp sync failed (%d)\n", rv);
            wait = retry;
            retry = retry * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : retry * 2;
        }
    }
}

void timesync_start(int port)
{
    main_port = port;
    wake_sem = gz_sem_create(0);
    if (wake_sem < 0 || gz_thread_create(worker, NULL) < 0)
        puts("springboard: can't start the ntp thread");
}

void timesync_wake(void)
{
    if (wake_sem >= 0)
        gz_sem_post(wake_sem);
}
