/* Remove only the CE NAND files staged by this debugger. All writes go
 * through CE's filesystem, which maintains its own NAND tags and ECC. */
#include "pcconnect.h"
#include <stdio.h>

static const char root[] = "\\Nand Disk\\GlazeUSB";
static const char *const names[] = {
    "UsbProbe.exe", "usb-probe.txt", "GlazeBoot.exe", "kernel.bin",
    "romfs.img", "glaze.cfg", "glazeboot.log"
};
static int file_op(struct pc_link *p, unsigned op, const char *path)
{
    uint8_t b[600];
    int n = pc_path(b, sizeof(b), path);
    return n < 0 ? -1 : pc_call(p, op, b, (unsigned)n);
}
static int attributes(struct pc_link *p, const char *path, uint32_t *attr)
{
    /* SDK WilGetFileAttributes: PC Connect opcode 0313. */
    if (file_op(p, 0x0313, path) < 18) return -1;
    if (p->result == ~0u) return p->error == 2 || p->error == 3 ? 0 : -1;
    *attr = p->result; return 1;
}
int pc_clean_nand(struct pc_link *p, int dry_run)
{
    uint32_t attr; int exists = attributes(p, root, &attr);
    if (exists < 0) return -1;
    if (!exists) { puts("No NAND debugger staging folder remains."); return 0; }
    if (!(attr & 0x10)) { fputs("NAND staging path is not a directory; stopped.\n", stderr); return -1; }
    unsigned present = 0, count = 0;
    char path[300];
    /* Inspect every candidate before deleting anything. No recursion or
     * wildcard deletion: unfamiliar files and subdirectories are preserved. */
    for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s\\%s", root, names[i]);
        exists = attributes(p, path, &attr);
        if (exists < 0 || (exists && (attr & 0x10))) {
            fprintf(stderr, "Cannot safely inspect %s; stopped before deletion.\n", path); return -1;
        }
        if (exists) { present |= 1u << i; count++; printf("NAND debugger file: %s\n", path); }
    }
    if (dry_run) { printf("Dry run: %u files selected; nothing removed.\n", count); return 0; }
    for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
        if (!(present & (1u << i))) continue;
        snprintf(path, sizeof(path), "%s\\%s", root, names[i]);
        if (file_op(p, 0x030e, path) < 18 || !p->result) {
            fprintf(stderr, "CE could not delete %s (error %u).\n", path, p->error); return -1;
        }
        if (attributes(p, path, &attr) != 0) {
            fprintf(stderr, "Deletion could not be verified: %s\n", path); return -1;
        }
        printf("Removed and verified: %s\n", path);
    }
    /* SDK WilRemoveDirectory (031a) only removes an empty directory. */
    if (file_op(p, 0x031a, root) < 18) return -1;
    if (!p->result) {
        if (p->error == 145) {
            printf("Removed %u debugger files; folder retained because other files remain.\n", count);
            return 0;
        }
        fprintf(stderr, "CE could not remove empty staging folder (error %u).\n", p->error); return -1;
    }
    if (attributes(p, root, &attr) != 0) return -1;
    printf("NAND debugger staging folder removed (%u files). CF files preserved.\n", count);
    return 0;
}
