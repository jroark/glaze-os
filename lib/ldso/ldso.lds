OUTPUT_FORMAT("elf32-tradlittlemips")
OUTPUT_ARCH(mips)
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x0f000000;
    .text : { *(.text) *(.text.*) } :text
    .rodata : { *(.rodata) *(.rodata.*) } :text
    . = ALIGN(4096);
    .data : { *(.data) *(.data.*) *(.sdata) *(.sdata.*) } :data
    .bss : { *(.bss) *(.bss.*) *(.sbss) *(.sbss.*) *(COMMON) } :data
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.note.*) *(.eh_frame) *(.pdr) *(.mdebug.abi32) *(.comment) }
}
