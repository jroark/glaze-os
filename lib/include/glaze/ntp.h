/*
 * SNTP client (libc, on glaze/net.h). The RTC keeps local time, so
 * ntp_sync converts through the zone chosen in Settings (glaze/tz.h).
 * SpringBoard syncs in the background while TZ_PREF_AUTO is on.
 */
#ifndef GLAZE_NTP_H
#define GLAZE_NTP_H

#include <glaze/types.h>

#define NTP_SERVER  "pool.ntp.org"

/* Asks an NTP server for the time: UTC glaze/notify.h seconds in *utc.
 * 0 or a negative GZ_E* code. */
int ntp_query(const char *host, int timeout_ms, uint32_t *utc);
/* Sets the RTC from NTP_SERVER in the current zone and records the time
 * in TZ_PREF_SYNCED. 0 or a negative GZ_E* code. */
int ntp_sync(void);

#endif
