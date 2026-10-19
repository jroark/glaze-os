/* Native validation tests: malformed arithmetic must fail before any ELF access. */
#include <glaze/elf.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    struct gz_elf_ehdr e = { .ident = {0x7f,'E','L','F',1,1,1}, .type=3, .machine=8,
        .version=1, .ehsize=52, .phoff=52, .phentsize=32, .phnum=1, .flags=0x10001007 };
    struct gz_elf_ehdr good = e;
    assert(gz_elf_header(&e, 84));
    for (unsigned n = 0; n < 84; n++) assert(!gz_elf_header(&e, n));
    e.phoff = UINT32_MAX; assert(!gz_elf_header(&e, UINT32_MAX)); e = good;
    e.phoff = 53; assert(!gz_elf_header(&e, 4096)); e = good;
    e.phnum = 33; assert(!gz_elf_header(&e, 4096)); e = good;
    e.flags |= 0x04000000; assert(!gz_elf_header(&e, 84)); e = good;
    e.ident[5] = 2; assert(!gz_elf_header(&e, 84)); e = good;
    e.machine = 3; assert(!gz_elf_header(&e, 84));
    struct gz_elf_phdr p = {.type=1, .offset=4096, .vaddr=8192, .filesz=16, .memsz=4096, .flags=5, .align=4096};
    assert(gz_elf_segment(&p, 4112));
    p.filesz=4097; assert(!gz_elf_segment(&p, 8193)); p.filesz=16;
    p.offset=UINT32_MAX; assert(!gz_elf_segment(&p, UINT32_MAX)); p.offset=4096;
    p.memsz=UINT32_MAX; assert(!gz_elf_segment(&p, 4112)); p.memsz=4096;
    p.vaddr=0x7ffff000u; p.memsz=8192; assert(!gz_elf_segment(&p, 4112)); p.vaddr=8192; p.memsz=4096;
    p.align=3; assert(!gz_elf_segment(&p, 4112)); p.align=4096;
    p.type=7; assert(!gz_elf_segment(&p, 4112));
    assert(!gz_elf_range(UINT32_MAX, 1, UINT32_MAX));
    puts("elf: ALL PASS");
    return 0;
}
