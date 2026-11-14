/* Partial link (ld -r) of a drivers/*.ko module. The driver objects are built
 * with the kernel's -ffunction-sections -fdata-sections, which leaves one
 * section per function and variable; kernel/module.c loads at most SECTIONS
 * (64) of them, and wl54cf.ko had 71. Merge them, keeping relocations. */
SECTIONS
{
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.* COMMON) }
}
