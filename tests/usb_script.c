#include "../host/glaze_usb/client.h"
#include "../host/glaze_usb/script.h"
#include <assert.h>
#include <glaze/abi.h>
#include <stdlib.h>
#include <string.h>
int gu_call(struct gu_client *c, uint32_t op, const void *p, uint32_t n)
{
    (void)c;
    (void)op;
    (void)p;
    (void)n;
    return -1;
}
int main(int argc, char **argv)
{
    uint8_t events[GU_SCRIPT_MAX * 12];
    unsigned n, line;
    assert(
        !usb_script_compile("# test\r\n0 tap 1 2\n20 text ab\n100 drag -2 3 251 331 60\n200 key enter # ok\n",
                            events, &n, &line));
    for (unsigned i = 1; i < n; i++)
        assert(gu_get32(events + (i - 1) * 12) <= gu_get32(events + i * 12));
    assert(events[4] == GZ_EV_TOUCH_DOWN && events[12 + 4] == GZ_EV_TEXT);
    const char *bad[] = {"0 tap 1",         "0 key nope",     "0 drag 1 2 3 4 -1", "-1 tap 0 0",
                         "3600000 tap 1 2", "0 tap 32768 2",  "0 tap 1 2 junk",    "0 drag 1 2 3 4 3600000",
                         "0 key enterx",    "0 text \xc3\xa9"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(usb_script_compile(bad[i], events, &n, &line) < 0);
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        assert(f);
        char text[65536];
        size_t k = fread(text, 1, sizeof(text) - 1, f);
        assert(feof(f));
        fclose(f);
        text[k] = 0;
        if (usb_script_compile(text, events, &n, &line)) {
            fprintf(stderr, "%s:%u invalid\n", argv[i], line);
            return 1;
        }
    }
    uint8_t *pixels = calloc(1, GU_SCREEN_BYTES);
    assert(pixels);
    pixels[0] = 0;
    pixels[1] = 0xf8;
    pixels[2] = 0xe0;
    pixels[3] = 7;
    pixels[4] = 31;
    FILE *f = fopen("../out/host/usb-screen-test.png", "wb");
    assert(f);
    assert(!usb_write_png(f, pixels));
    assert(!fclose(f));
    free(pixels);
    puts("usb-script and PNG: ALL PASS");
    return 0;
}
