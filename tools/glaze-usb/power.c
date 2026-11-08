/* `power`: battery status, and powering off with an RTC-alarm wake, so sleep
 * and wake can be tested without pressing Power (tools/hwtest). */
#include "client.h"
#include <glaze/abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *card_power_name(uint32_t v)
{
    static const char *names[] = { "none", "full", "power save", "doze", "radio off", "off" };
    return v < sizeof(names) / sizeof(names[0]) ? names[v] : "?";
}

int gu_usb_detach(struct gu_client *c, const char *ms_text)
{
    if (!(c->caps & GU_CAP_DETACH)) {
        fputs("This kernel can't leave the bus; reload a newer build.\n", stderr);
        return -1;
    }
    uint8_t b[4];
    uint32_t ms = (uint32_t)strtoul(ms_text, NULL, 0);
    gu_put32(b, ms);
    if (gu_call(c, GU_USB_DETACH, b, 4) < 0) return -1;
    fprintf(stderr, "The device's USB unit goes dead for %u ms.\n", ms);
    return 0;
}

int gu_power(struct gu_client *c, int argc, char **argv)
{
    if (!(c->caps & GU_CAP_POWER)) {
        fputs("This kernel has no USB power control; reload a newer build.\n", stderr);
        return -1;
    }
    uint8_t b[12];
    if (!strcmp(argv[0], "status")) {
        gu_put32(b, GU_POWER_STATUS);
        int n = gu_call(c, GU_POWER, b, 4);
        if (n < (int)GZ_POWER_STATUS_V1_SIZE) return -1;
        struct gz_power_status st;
        memset(&st, 0, sizeof(st));
        memcpy(&st, c->frame + GU_HEADER, (size_t)n < sizeof(st) ? (size_t)n : sizeof(st));
        printf("flags=0x%x percent=%u level=%u adc=%u/%u cal=%u-%u samples=%u idle_ms=%u standby=%u\n",
               st.flags, st.percent, st.level, st.adc_avg, st.adc_last, st.cal_min, st.cal_max, st.samples,
               st.idle_ms, st.standby_count);
        if (n >= (int)sizeof(st))
            printf("card=%s full_ms=%u save_ms=%u doze_ms=%u off_ms=%u net_irqs=%u\n",
                   card_power_name(st.card_power), st.card_full_ms, st.card_save_ms, st.card_doze_ms,
                   st.card_off_ms, st.net_irqs);
        return 0;
    }
    uint32_t op = !strcmp(argv[0], "suspend") ? GU_POWER_SUSPEND : !strcmp(argv[0], "hibernate") ? GU_POWER_HIBERNATE
                : !strcmp(argv[0], "idle") ? GU_POWER_IDLE : 0;
    if (!op || argc < 2) return -1;
    uint32_t seconds = (uint32_t)strtoul(argv[1], NULL, 0);
    uint32_t idle_ms = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 10000;
    gu_put32(b, op);
    gu_put32(b + 4, seconds);
    gu_put32(b + 8, op == GU_POWER_IDLE ? idle_ms : 0);
    if (gu_call(c, GU_POWER, b, 12) < 0) return -1;
    if (op == GU_POWER_IDLE)
        fprintf(stderr, "Idle power off armed: after %u ms without activity once this debugger disconnects; "
                        "the alarm wakes it %u s from now.\n", idle_ms, seconds);
    else
        fprintf(stderr, "Powering off (%s); the alarm wakes it in %u s.\n", argv[0], seconds);
    return 0;
}
