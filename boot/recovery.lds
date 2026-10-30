OUTPUT_FORMAT("elf32-tradlittlemips")
OUTPUT_ARCH(mips)
ENTRY(_start)
SECTIONS {
    . = 0x80200000;
    .text : { *(.text) *(.text.*) }
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.gnu.attributes) *(.note*) *(.pdr) *(.mdebug*) }
}
