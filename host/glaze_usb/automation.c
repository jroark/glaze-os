#include "client.h"
#include "script.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int gu_script(struct gu_client *c, const char *path, const char *screenshot)
{
    if (!(c->caps & GU_CAP_SCRIPT)) {
        fputs("This kernel needs the USB script update.\n", stderr);
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }
    char *text = malloc(256 * 1024 + 1);
    uint8_t *events = malloc(GU_SCRIPT_MAX * 12);
    if (!text || !events) {
        fclose(f);
        free(text);
        free(events);
        return -1;
    }
    size_t n = fread(text, 1, 256 * 1024, f);
    int bad = ferror(f) || !feof(f);
    fclose(f);
    text[n] = 0;
    unsigned count = 0, line = 0;
    if (bad || memchr(text, 0, n) || usb_script_compile(text, events, &count, &line)) {
        fprintf(stderr, "Invalid or oversized input script: %s (line %u)\n", path, line);
        free(text);
        free(events);
        return -1;
    }
    free(text);
    uint8_t b[GU_PAYLOAD];
    gu_put32(b, count);
    int result = -1;
    if (gu_call(c, GU_SCRIPT_BEGIN, b, 4) < 0)
        goto done;
    for (unsigned off = 0; off < count;) {
        unsigned k = count - off > 340 ? 340 : count - off;
        gu_put32(b, off);
        memcpy(b + 4, events + off * 12, k * 12);
        if (gu_call(c, GU_SCRIPT_DATA, b, 4 + k * 12) < 0)
            goto cancel;
        off += k;
    }
    if (gu_call(c, GU_SCRIPT_START, NULL, 0) < 0)
        goto cancel;
    fprintf(stderr, "Running %s: %u timed events\n", path, count);
    while (!usb_cancelled) {
        int r = gu_call(c, GU_SCRIPT_STATUS, NULL, 0);
        if (r != 16)
            goto cancel;
        const uint8_t *s = c->frame + GU_HEADER;
        if (gu_get32(s) == GU_SCRIPT_DONE) {
            fprintf(stderr, "Script complete: %u/%u events, %u ms\n", gu_get32(s + 8), gu_get32(s + 12),
                    gu_get32(s + 4));
            usleep(350000);
            result = screenshot ? gu_screenshot(c, screenshot) : 0;
            goto done;
        }
        if (gu_get32(s) != GU_SCRIPT_RUNNING)
            goto cancel;
        usleep(100000);
    }
cancel:
    gu_call(c, GU_SCRIPT_CANCEL, NULL, 0);
done:
    free(events);
    return result;
}
