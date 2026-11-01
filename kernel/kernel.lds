/* Glaze kernel: KSEG0, loaded flat by the SPL at 0x80010000. */

OUTPUT_FORMAT("elf32-tradlittlemips")
OUTPUT_ARCH(mips)
ENTRY(_start)

SECTIONS {
    . = 0x80010000;
    __text_start = .;
    .text : {
        *(.text.start)
        *(.text)
        *(.text.*)
    }
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }
    /* The text and read-only data end: the SPL checks them before it
     * resumes a HIBERNATE (kernel/resume.h). */
    . = ALIGN(4);
    __ro_end = .;
    .data : {
        *(.data)
        *(.data.*)
        *(.sdata)
        *(.sdata.*)
    }
    . = ALIGN(16);
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.sbss)
        *(.sbss.*)
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    . = ALIGN(4096);
    __kernel_end = .;
    /DISCARD/ : {
        *(.reginfo)
        *(.MIPS.abiflags)
        *(.gnu.attributes)
        *(.note.gnu.build-id)
        *(.note.GNU-stack)
        *(.eh_frame)
        *(.pdr)
        *(.mdebug.abi32)
        *(.comment)
    }
}
