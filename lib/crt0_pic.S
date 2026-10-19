/* O32 PIC entry, t9 supplied by the kernel/interpreter. */
    .abicalls
    .set noreorder
    .text
    .globl _start
    .ent _start
_start:
    .cpload $25
    addiu $sp, $sp, -32
    lw $25, %call16(__libc_start)($gp)
    jalr $25
    nop
1:  b 1b
    nop
    .end _start

    .section .note.GNU-stack,"",@progbits
