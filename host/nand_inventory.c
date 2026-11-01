/* Local raw-image inspection and encoding. Never opens a hardware device. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/drivers/nand_boot.h"
#include "../kernel/drivers/nand_ecc.h"
static int raw_read(void *ctx, uint32_t p, uint8_t raw[528])
{
    FILE *f = ctx;
    return fseek(f, (long)p * 528, SEEK_SET) || fread(raw,1,528,f) != 528 ? -1 : 0;
}
int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc == 2 && !strcmp(argv[1], "--encode")) {
        uint8_t raw[528]; size_t n;
        while ((n=fread(raw,1,520,stdin))) {
            if (n != 520) return 2;
            nand_ecc_encode(raw);
            if (fwrite(raw,1,528,stdout) != 528) return 2;
        }
        return ferror(stdin) ? 2 : 0;
    }
    bool check = argc == 3 && !strcmp(argv[1], "--check-boot");
    if (!(argc == 2 || check)) return 2;
    FILE *f = fopen(argv[check ? 2 : 1], "rb");
    if (!f || fseek(f,0,SEEK_END) || ftell(f) != 1024L*32*528) return 2;
    static struct nand_boot_map map;
    if (nand_boot_scan(&map,raw_read,f,false)) return 1;
    if (check) {
        uint8_t data[512];
        for (unsigned p=0;p<NAND_BOOT_BLOCKS*32;p++)
            if (nand_boot_read(&map,raw_read,f,p,data,false)) return 1;
    } else if (fwrite(map.quarantine,1,128,stdout) != 128) return 2;
    fclose(f);
    return 0;
}
