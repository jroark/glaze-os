/* Glaze user programs: static, linked at 0x00400000 in kuseg. */
OUTPUT_FORMAT("elf32-tradlittlemips")
OUTPUT_ARCH(mips)
ENTRY(_start)

SECTIONS {
    . = 0x00400000;
    .text : { *(.text._start) *(.text) *(.text.*) }
    .rodata : { *(.rodata) *(.rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data) *(.data.*) *(.sdata) *(.sdata.*) }
    .bss : { *(.sbss) *(.sbss.*) *(.bss) *(.bss.*) *(COMMON) }
    _end = .;
    /DISCARD/ : {
        *(.reginfo) *(.MIPS.abiflags) *(.gnu.attributes) *(.note.*)
        *(.eh_frame) *(.pdr) *(.mdebug.abi32) *(.comment)
    }
}
