#include "transport.h"
#include "pcconnect.h"
#include "client.h"
#include "network.h"
#include <glaze/debug_net.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>

static int number(const char *s, uint32_t *out)
{
    char *end;
    errno=0;
    unsigned long long value=strtoull(s,&end,0);
    if (errno || !*s || *s=='-' || *end || value>UINT32_MAX) return -1;
    *out=(uint32_t)value;
    return 0;
}

static void stop(int signum) { (void)signum; usb_cancelled = 1; }
static uint64_t monotonic_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (unsigned)t.tv_nsec / 1000000;
}
static int reconnect(struct usb_link *l, const char *device, struct gu_client *c, const char *host, const char *key)
{
    if (host) {
        usb_close(l);
        uint64_t end=monotonic_ms()+90000;
        fputs("Waiting for paired network debugger (up to 90 seconds)...\n",stderr);
        while (!usb_cancelled && monotonic_ms()<end) {
            usleep(500000);
            if (!debug_host_open(l,host,key) && !gu_hello(c,l)) return 0;
            usb_close(l);
        }
        return -1;
    }
    char location[64];
    int n = snprintf(location, sizeof(location), "%u", l->bus);
    for (unsigned j = 0; j < l->port_count; j++) n += snprintf(location + n, sizeof(location) - (size_t)n, ".%u", l->ports[j]);
    usb_close(l);
    uint64_t until = monotonic_ms() + 90000, reset_at = monotonic_ms() + 3000;
    fputs("Waiting for Glaze USB (up to 90 seconds)...\n", stderr);
    while (monotonic_ms() < until && !usb_cancelled) {
        usleep(500000);
        if (!usb_open(l, device ? device : location, 0)) {
            if (l->revision == 0x0100 && !gu_hello(c, l)) return 0;
            usb_close(l);
        }
        if (monotonic_ms() >= reset_at) {
            /* CE can power down USB while GlazeBoot is still reading CF.
             * Retry re-enumeration after the new kernel has had time to
             * start, rather than resetting only during the old CE session.
             * Keep the interface unclaimed during the host reset. */
            usb_reset_link(device ? device : location, l->trace);
            reset_at = monotonic_ms() + 5000;
        }
    }
    fprintf(stderr, "The new Glaze kernel did not reconnect. If SpringBoard is running, replug USB once and run probe.\n"); return -1;
}
static int usage(void)
{
    fputs("usage: glaze-usb [--trace] [--cf-only] [--device BUS.PORT | --host HOST[:PORT] --key FILE] COMMAND [ARGS]\n"
          "  net-keygen KEYFILE             create a private pairing record offline\n"
          "  net-enable KEYFILE             provision and enable network debugging over USB\n"
          "  net-disable                    disable the network listener\n"
          "  usb-net [INTERFACE]            USB IP link (macOS utun / Linux TAP)\n"
          "  mirror [--view-only] [--port N] [--fps N]  local VNC screen sharing\n"
          "  probe                         descriptors and runtime\n"
          "  ce-probe                      stock CE handshake\n"
          "  ce-info                       report CE staging-volume space\n"
          "  ce-clean-nand [--dry-run]      remove debugger staging files from CE NAND\n"
          "  ce-self-test [ARTIFACT_DIR]    harmless upload/download/launch check\n"
          "  reset-link                    reset USB bus connection\n"
          "  diag                          Glaze control/status diagnostics\n"
          "  peek PA [COUNT]               HWDUMP=1 kernels: words of RAM/companion/VR4131 over EP0\n"
          "  peek-dump PA WORDS OUTPUT     the same, saved little-endian to a file\n"
          "  poke PA VALUE [16]            HWDUMP=1 kernels: write a word (or halfword), print it back\n"
          "  put LOCAL REMOTE              verified upload\n"
          "  get REMOTE LOCAL              download\n"
          "  run REMOTE [CE_ARGS]          launch CE executable / Glaze package or built-in\n"
          "  boot [ARTIFACT_DIR]            CE -> Glaze; default out/glazeboot\n"
          "  reload [ARTIFACT_DIR]          fresh Glaze RAM boot\n"
          "  reboot [--ce|--glaze]          restart current OS, or select the target\n"
          "  logs [--once]                 stream/drain Glaze console\n", stderr);
    fputs("  nand-backup OUTPUT.raw         two-pass physical NAND backup (read-only)\n"
          "  nand-status                   read chip ready/failure/write-protect bits\n"
          "  nand-apply SOURCE TARGET PLAN --execute  raw NAND program/verify (repair boot only)\n"
          "  cf-info                       CF card geometry (read-only)\n"
          "  cf-read LBA COUNT OUTPUT       two-pass raw CF extent snapshot (read-only)\n"
          "  cf-format-recovery SECTORS GENERATION  erase CF filesystem; create recovery FAT16\n"
          "  screenshot OUTPUT.png          capture the displayed 240x320 frame\n"
          "  script INPUT.txt [OUTPUT.png]  replay emulator-style timed input\n"
          "  connect                       hold a debugger session; Ctrl-C closes\n"
          "  run REMOTE --wait             launch and stay connected until app exit\n", stderr);
    return 2;
}
int main(int argc, char **argv)
{
    struct usb_link l = {0}; const char *device = NULL, *host=NULL, *key=NULL;
    int i = 1, cf_only = 0;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "--trace")) l.trace = 1;
        else if (!strcmp(argv[i], "--cf-only")) cf_only = 1;
        else if (!strcmp(argv[i], "--host") && i+1<argc) host=argv[++i];
        else if (!strcmp(argv[i], "--key") && i+1<argc) key=argv[++i];
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) device = argv[++i];
        else break;
        i++;
    }
    if (i == argc || (device && host) || (key && !host)) return usage();
    const char *cmd = argv[i++]; int left = argc - i;
    if (cf_only && strcmp(cmd,"boot") && strcmp(cmd,"ce-self-test")) return usage();
    if (!strcmp(cmd,"net-keygen")) return left==1 && !host && !device ? (debug_host_keygen(argv[i]) ? 1 : 0) : usage();
    if ((!strcmp(cmd, "probe") || !strcmp(cmd, "ce-probe") || !strcmp(cmd, "ce-info") || !strcmp(cmd, "reset-link") || !strcmp(cmd, "diag") || !strcmp(cmd, "connect") || !strcmp(cmd,"net-disable") || !strcmp(cmd,"cf-info") || !strcmp(cmd,"nand-status")) ? left != 0 :
        !strcmp(cmd,"cf-read") ? left != 3 :
        !strcmp(cmd,"peek") ? (left < 1 || left > 2) :
        !strcmp(cmd,"peek-dump") ? left != 3 :
        !strcmp(cmd,"poke") ? (left < 2 || left > 3 || (left == 3 && strcmp(argv[i + 2], "16"))) :
        !strcmp(cmd,"cf-format-recovery") ? left != 2 :
        !strcmp(cmd,"nand-apply") ? (left!=4 || strcmp(argv[i+3],"--execute")) :
        (!strcmp(cmd, "screenshot") || !strcmp(cmd,"net-enable") || !strcmp(cmd,"nand-backup")) ? left != 1 :
        !strcmp(cmd, "script") ? (left < 1 || left > 2) :
        !strcmp(cmd, "usb-net") ? left > 1 :
        !strcmp(cmd, "mirror") ? 0 :
        !strcmp(cmd, "reboot") ? (left > 1 || (left == 1 && strcmp(argv[i], "--ce") && strcmp(argv[i], "--glaze"))) :
        !strcmp(cmd, "ce-clean-nand") ? (left > 1 || (left == 1 && strcmp(argv[i], "--dry-run"))) :
        (!strcmp(cmd, "put") || !strcmp(cmd, "get")) ? left != 2 :
        !strcmp(cmd, "run") ? (left < 1 || left > 2) :
        (!strcmp(cmd, "boot") || !strcmp(cmd, "reload") || !strcmp(cmd, "ce-self-test")) ? left > 1 :
        !strcmp(cmd, "logs") ? (left > 1 || (left == 1 && strcmp(argv[i], "--once"))) : 1) return usage();
    signal(SIGINT, stop); signal(SIGTERM, stop);
    signal(SIGPIPE,SIG_IGN);
    if (host && (!strncmp(cmd,"ce-",3) || !strcmp(cmd,"reset-link") || !strcmp(cmd,"diag") || !strncmp(cmd,"peek",4) || !strcmp(cmd,"poke") || !strcmp(cmd,"net-enable") || !strcmp(cmd,"usb-net") || !strcmp(cmd,"cf-format-recovery") || !strcmp(cmd,"nand-apply"))) {
        fputs("This command requires the USB transport.\n",stderr); return 2;
    }
    if (!strcmp(cmd, "reset-link")) return usb_reset_link(device, l.trace) ? 1 : 0;
    if (host ? debug_host_open(&l,host,key) : usb_open(&l, device, !strcmp(cmd, "probe"))) return 1;
    struct gu_client c; int r = 0;
    if (!strcmp(cmd, "diag")) {
        r = -1;
        if (l.revision == 0x0100 && usb_lines(&l, 3) >= 0) {
            uint8_t b[8]; int done;
            for (unsigned page = 0; page < 2; page++) {
                r = libusb_control_transfer(l.dev, 0x40, 0x5a, (uint16_t)page, 0, NULL, 0, 1000);
                if (r < 0) break;
                r = libusb_interrupt_transfer(l.dev, 0x83, b, sizeof(b), &done, 1000);
                if (r || done != 8 || b[0] != 'G' || b[1] != 'Z') { r = -1; break; }
                if (!page) printf("hardware: DMA=%02x INT0=%02x INT1=%02x INT2=%02x EP=%02x RXlen=%u\n", b[2], b[3], b[4], b[5], b[6], b[7]);
                else printf("software counters (mod 256): RX=%u/%u TX=%u/%u flags=%02x generation=%u\n", b[2], b[3], b[4], b[5], b[6], b[7]);
            }
        }
        if (r) fputs("USB diagnostics unavailable\n", stderr);
    } else if (!strcmp(cmd, "poke")) {
        /* Bring-up experiments on HWDUMP=1 kernels: load the value with
         * request 0x5b, then write it through the peek address encoding. */
        uint32_t pa = (uint32_t)strtoul(argv[i], NULL, 0), value = (uint32_t)strtoul(argv[i + 1], NULL, 0);
        unsigned region = pa < 0x01000000u ? 0 : (pa >> 24) == 0x0A ? 1 : (pa >> 24) == 0x0F ? 2 : 3;
        uint32_t index = (pa & 0x00FFFFFFu) >> 2;
        int half = left == 3;
        uint8_t b[8]; int done;
        r = -1;
        if (region > 2 || (pa & (half ? 1 : 3)) || index >= (1u << 22)) fprintf(stderr, "poke: bad address %08x\n", pa);
        else if (l.revision != 0x0100 || usb_lines(&l, 3) < 0) fputs("poke: the device isn't running Glaze\n", stderr);
        else if (libusb_control_transfer(l.dev, 0x40, 0x5b, (uint16_t)value, (uint16_t)(value >> 16), NULL, 0, 1000) >= 0) {
            libusb_interrupt_transfer(l.dev, 0x83, b, sizeof(b), &done, 1000);    /* the line state */
            uint16_t sel = (uint16_t)((half ? 4 : 3) | (region << 6 | index >> 16) << 8);
            /* A halfword at +2 is the word's upper half: the kernel writes
             * 16 bits at the word address, so pass the exact address. */
            uint16_t lo = (uint16_t)(half ? (pa & 0x00FFFFFFu) >> 2 : index);
            if (half && (pa & 2)) { fputs("poke: 16-bit writes need a 4-byte aligned address\n", stderr); }
            else if (libusb_control_transfer(l.dev, 0x40, 0x5a, sel, lo, NULL, 0, 1000) >= 0 &&
                     !libusb_interrupt_transfer(l.dev, 0x83, b, sizeof(b), &done, 1000) &&
                     done == 8 && b[0] == 'G' && b[1] == 'Z' && b[6] == 'W') {
                printf("%08x <- %08x, reads %02x%02x%02x%02x\n", pa, value, b[5], b[4], b[3], b[2]);
                r = 0;
            } else fputs("poke: no answer (not an HWDUMP=1 kernel?)\n", stderr);
        }
    } else if (!strcmp(cmd, "peek") || !strcmp(cmd, "peek-dump")) {
        /* Read-only words through the out-of-band diagnostic request, which
         * works while bulk transfers don't (kernel/drivers/usb.c). Only
         * HWDUMP=1 kernels answer. A gated companion unit hangs the device's
         * bus, so ask only for addresses known to be live. */
        uint32_t pa = (uint32_t)strtoul(argv[i], NULL, 0);
        uint32_t count = left > 1 ? (uint32_t)strtoul(argv[i + 1], NULL, 0) : 1;
        FILE *out = NULL;
        if (!strcmp(cmd, "peek-dump") && !(out = fopen(argv[i + 2], "wb"))) { perror(argv[i + 2]); return 1; }
        r = l.revision == 0x0100 && usb_lines(&l, 3) >= 0 ? 0 : -1;
        if (r) fputs("peek: the device isn't running Glaze\n", stderr);
        for (uint32_t k = 0; !r && k < count; k++, pa += 4) {
            unsigned region = pa < 0x01000000u ? 0 : (pa >> 24) == 0x0A ? 1 : (pa >> 24) == 0x0F ? 2 : 3;
            uint32_t index = (pa & 0x00FFFFFFu) >> 2;
            uint8_t b[8]; int done;
            if (region > 2 || (pa & 3) || index >= (1u << 22)) { fprintf(stderr, "peek: bad address %08x\n", pa); r = -1; break; }
            uint16_t value = (uint16_t)(2 | (region << 6 | index >> 16) << 8);
            r = libusb_control_transfer(l.dev, 0x40, 0x5a, value, (uint16_t)index, NULL, 0, 1000);
            if (r < 0) break;
            r = libusb_interrupt_transfer(l.dev, 0x83, b, sizeof(b), &done, 1000);
            if (r || done != 8 || b[0] != 'G' || b[1] != 'Z' || b[6] != 'P') { fputs("peek: no answer (not an HWDUMP=1 kernel?)\n", stderr); r = -1; break; }
            if (out) fwrite(b + 2, 1, 4, out);
            else printf("%08x: %02x%02x%02x%02x\n", pa, b[5], b[4], b[3], b[2]);
        }
        if (out) fclose(out);
    } else if (!strcmp(cmd, "probe")) {
        if (l.revision == 0x0100) r = gu_hello(&c, &l);
        else puts("  stock CE descriptor (use ce-probe to establish a session)");
    } else if (!strcmp(cmd, "reboot")) {
        int glaze = left ? !strcmp(argv[i], "--glaze") : l.revision == 0x0100;
        if (l.revision == 0x0100) {
            r = gu_hello(&c, &l);
            if (!r && glaze) {
                r = gu_reload(&c, "out/glazeboot");
                if (!r) r = reconnect(&l, device, &c, host, key);
                if (!r) r = gu_logs(&c, 1);
            } else if (!r) {
                if (!(c.caps & GU_CAP_REBOOT_CE)) {
                    fputs("This kernel needs the debugger CE reboot update; use reload first.\n", stderr); r = -1;
                } else {
                    r = gu_call(&c, GU_REBOOT_CE, NULL, 0);
                    if (!r) fputs("Windows CE cold reboot accepted; volatile RAM will be cleared.\n", stderr);
                }
            }
        } else if (glaze) {
            r = ce_boot(&l, "out/glazeboot", 0);
            if (!r) r = reconnect(&l, device, &c, host, key);
            if (!r) r = gu_logs(&c, 1);
        } else {
            struct pc_link p; r = pc_connect(&p, &l);
            if (!r) r = pc_reboot(&p);
        }
    } else if (!strcmp(cmd, "ce-clean-nand")) {
        if (l.revision == 0x0100) { fputs("Reboot to Windows CE to safely clean its NAND filesystem.\n", stderr); r = -1; }
        else {
            struct pc_link p; r = pc_connect(&p, &l);
            uint64_t before = 0, after = 0;
            if (!r) {
                int have_before = !pc_free_space(&p, "\\Nand Disk", &before);
                r = pc_clean_nand(&p, left == 1);
                if (!r && have_before && !pc_free_space(&p, "\\Nand Disk", &after))
                    printf("NAND free: %llu -> %llu bytes (%llu bytes reclaimed).\n", (unsigned long long)before,
                           (unsigned long long)after, (unsigned long long)(after >= before ? after - before : 0));
            }
        }
    } else if (!strcmp(cmd, "ce-probe") || !strcmp(cmd, "ce-info")) {
        if (l.revision == 0x0100) { fputs("Device is running Glaze\n", stderr); r = -1; }
        else {
            struct pc_link p; r = pc_connect(&p, &l);
            if (!r && !strcmp(cmd, "ce-info")) {
                const char *roots[] = {"\\Storage Card", "\\Nand Disk"};
                for (unsigned k = 0; k < 2; k++) {
                    uint64_t free_bytes;
                    if (!pc_free_space(&p, roots[k], &free_bytes)) printf("%s free=%llu bytes\n", roots[k], (unsigned long long)free_bytes);
                    else printf("%s unavailable (CE error %u)\n", roots[k], p.error);
                }
            }
        }
    } else if (!strcmp(cmd, "ce-self-test")) {
        r = l.revision == 0x0100 ? -1 : ce_self_test(&l, left ? argv[i] : "out/glazeboot", cf_only);
    } else if (!strcmp(cmd, "boot") && l.revision != 0x0100) {
        r = ce_boot(&l, left ? argv[i] : "out/glazeboot", cf_only);
        if (!r) r = reconnect(&l, device, &c, host, key);
        if (!r) r = gu_logs(&c, 1);
    } else if (l.revision == 0x0100) {
        r = gu_hello(&c, &l);
        if (!r) {
            if (!strcmp(cmd,"mirror")) r=gu_mirror(&c,left,argv+i);
            else if (!strcmp(cmd,"usb-net")) r=gu_usb_net(&c,left ? argv[i] : NULL);
            else if (!strcmp(cmd,"net-enable")) {
                r=debug_host_key_valid(argv[i]);
                int trace=l.trace; l.trace=0; /* pairing material is never packet-traced */
                if (!r) r=gu_upload(&c,argv[i],"/Library/Preferences/" GD_CONFIG);
                l.trace=trace;
                uint8_t on[4]; gu_put32(on,1);
                if (!r) r=gu_call(&c,GU_NET_ENABLE,on,4);
            } else if (!strcmp(cmd,"net-disable")) {
                uint8_t off[4]={0}; r=gu_call(&c,GU_NET_ENABLE,off,4);
            } else if (!strcmp(cmd, "put")) r = gu_upload(&c, argv[i], argv[i + 1]);
            else if (!strcmp(cmd, "get")) r = gu_download(&c, argv[i], argv[i + 1]);
            else if (!strcmp(cmd, "run")) {
                r = left == 1 || !strcmp(argv[i+1], "--wait") ? gu_call(&c, GU_RUN, argv[i], (uint32_t)strlen(argv[i])) : -1;
                if (!r && left == 2) {
                    if (!(c.caps & GU_CAP_STATUS)) r = -1;
                    while (!r && !usb_cancelled) {
                        int n = gu_call(&c, GU_DEBUG_STATUS, NULL, 0);
                        if (n != 8) { r = -1; break; }
                        if (!gu_get32(c.frame + GU_HEADER + 4)) break;
                        usleep(250000);
                    }
                }
            }
            else if (!strcmp(cmd, "nand-backup")) r = gu_nand_backup(&c, argv[i]);
            else if (!strcmp(cmd,"nand-apply")) r=gu_nand_apply(&c,argv[i],argv[i+1],argv[i+2]);
            else if (!strcmp(cmd, "nand-status")) {
                r=gu_call(&c,GU_NAND_STATUS,NULL,0);
                if (r==4) {
                    unsigned status=gu_get32(c.frame+GU_HEADER);
                    printf("NAND chip status=%02x ready=%u failed=%u write_enabled=%u\n",
                           status,!!(status&0x40),!!(status&1),!!(status&0x80));
                    r=0;
                } else r=-1;
            }
            else if (!strcmp(cmd, "cf-info")) { uint32_t sectors,generation; r=gu_cf_info(&c,&sectors,&generation); }
            else if (!strcmp(cmd, "cf-format-recovery")) {
                uint32_t sectors,generation,current_sectors,current_generation;
                if (number(argv[i],&sectors) || number(argv[i+1],&generation) ||
                    gu_cf_info(&c,&current_sectors,&current_generation) ||
                    sectors!=current_sectors || generation!=current_generation) r=-1;
                else {
                    uint8_t b[12];gu_put32(b,sectors);gu_put32(b+4,generation);gu_put32(b+8,0x36314643u);
                    fprintf(stderr,"Formatting CF (%u sectors, generation %u) as recovery FAT16\n",sectors,generation);
                    r=gu_call(&c,GU_CF_FORMAT,b,sizeof(b));
                }
            }
            else if (!strcmp(cmd, "cf-read")) {
                uint32_t lba,count;
                if (number(argv[i],&lba) || number(argv[i+1],&count)) r=-1;
                else r=gu_cf_read(&c,lba,count,argv[i+2]);
            }
            else if (!strcmp(cmd, "screenshot")) r = gu_screenshot(&c, argv[i]);
            else if (!strcmp(cmd, "script")) r = gu_script(&c, argv[i], left == 2 ? argv[i+1] : NULL);
            else if (!strcmp(cmd, "connect")) {
                if (!(c.caps & GU_CAP_STATUS)) r = -1;
                unsigned previous = ~0u;
                while (!r && !usb_cancelled) {
                    int n = gu_call(&c, GU_DEBUG_STATUS, NULL, 0);
                    if (n != 8) { r = -1; break; }
                    unsigned flags = gu_get32(c.frame + GU_HEADER);
                    if (flags != previous) fprintf(stderr, "Debugger connected%s%s\n", flags & GU_DEBUG_RUN ? "; app running" : "", flags & GU_DEBUG_SCRIPT ? "; script running" : "");
                    previous = flags; usleep(250000);
                }
            }
            else if (!strcmp(cmd, "boot") || !strcmp(cmd, "reload")) {
                r = gu_reload(&c, left ? argv[i] : "out/glazeboot");
                if (!r) r = reconnect(&l, device, &c, host, key);
                if (!r) r = gu_logs(&c, 1);
            } else if (!strcmp(cmd, "logs")) {
                if (left) r = gu_logs(&c, 1);
                else while (!usb_cancelled && !(r = gu_logs(&c, 1))) usleep(20000);
            }
        }
    } else if (!strcmp(cmd, "put") || !strcmp(cmd, "get") || !strcmp(cmd, "run")) {
        struct pc_link p; r = pc_connect(&p, &l);
        if (!r) {
            if (!strcmp(cmd, "put")) r = pc_put(&p, argv[i], argv[i + 1]);
            else if (!strcmp(cmd, "get")) r = pc_get(&p, argv[i], argv[i + 1]);
            else r = pc_run(&p, argv[i], left > 1 ? argv[i + 1] : "");
        }
    } else { fputs("This command requires a running Glaze USB build.\n", stderr); r = -1; }
    usb_close(&l); return r ? 1 : 0;
}
