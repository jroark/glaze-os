/* Fixed-address, self-contained interpreter; compiled without ABI calls. */
    .set noreorder
    .text
    .globl _start
_start:
    addiu $sp, $sp, -32
    jal ld_start
    nop
1:  b 1b
    nop

/* Call arbitrary O32 PIC code with t9 = callee, and up to four arguments. */
    .globl ld_call
ld_call:
    addiu $sp, $sp, -32
    sw $ra, 28($sp)
    move $t9, $a0
    move $a0, $a1
    move $a1, $a2
    move $a2, $a3
    lw $a3, 48($sp)
    jalr $t9
    nop
    lw $ra, 28($sp)
    addiu $sp, $sp, 32
    jr $ra
    nop
