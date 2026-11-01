/* setjmp/longjmp for the SPL (boot/include/setjmp.h). */
    .set noreorder
    .text

    .globl setjmp
    .type setjmp, @function
setjmp:
    sw $16, 0($4)
    sw $17, 4($4)
    sw $18, 8($4)
    sw $19, 12($4)
    sw $20, 16($4)
    sw $21, 20($4)
    sw $22, 24($4)
    sw $23, 28($4)
    sw $30, 32($4)
    sw $29, 36($4)
    sw $31, 40($4)
    sw $28, 44($4)
    jr $31
    move $2, $0

    .globl longjmp
    .type longjmp, @function
longjmp:
    lw $16, 0($4)
    lw $17, 4($4)
    lw $18, 8($4)
    lw $19, 12($4)
    lw $20, 16($4)
    lw $21, 20($4)
    lw $22, 24($4)
    lw $23, 28($4)
    lw $30, 32($4)
    lw $29, 36($4)
    lw $31, 40($4)
    lw $28, 44($4)
    bne $5, $0, 1f
    move $2, $5
    li $2, 1            /* longjmp(env, 0) returns 1 */
1:  jr $31
    nop
