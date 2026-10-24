/* The trampoline alone, at 0, for tools/mktrampblob.py: tramp_entry first.
 * It is position independent, so the address only fixes symbol offsets. */

OUTPUT_FORMAT("elf32-tradlittlemips")
OUTPUT_ARCH(mips)
ENTRY(tramp_entry)

SECTIONS {
    . = 0;
    .text : {
        *(.tramp.entry)
        *(.tramp)
    }
    /DISCARD/ : {
        *(.text)
        *(.text.*)
        *(.data)
        *(.data.*)
        *(.bss)
        *(.rodata)
        *(.rodata.*)
        *(.reginfo)
        *(.MIPS.abiflags)
        *(.MIPS.options)
        *(.gnu.attributes)
        *(.note*)
        *(.eh_frame)
        *(.pdr)
        *(.mdebug.*)
        *(.comment)
    }
}
