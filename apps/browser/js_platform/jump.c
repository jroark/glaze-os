/* O32 callee-saved state used by the browser JavaScript parser and failure boundary. */
#ifdef __mips__
__asm__(".text\n.set noreorder\n.align 2\n"
".globl glaze_js_setjmp\n.ent glaze_js_setjmp\nglaze_js_setjmp:\n"
"sw $16,0($4)\nsw $17,4($4)\nsw $18,8($4)\nsw $19,12($4)\n"
"sw $20,16($4)\nsw $21,20($4)\nsw $22,24($4)\nsw $23,28($4)\n"
"sw $28,32($4)\nsw $29,36($4)\nsw $30,40($4)\nsw $31,44($4)\n"
"jr $31\nmove $2,$0\n.end glaze_js_setjmp\n"
".globl glaze_js_longjmp\n.ent glaze_js_longjmp\nglaze_js_longjmp:\n"
"lw $16,0($4)\nlw $17,4($4)\nlw $18,8($4)\nlw $19,12($4)\n"
"lw $20,16($4)\nlw $21,20($4)\nlw $22,24($4)\nlw $23,28($4)\n"
"lw $28,32($4)\nlw $29,36($4)\nlw $30,40($4)\nlw $31,44($4)\n"
"sltiu $2,$5,1\naddu $2,$2,$5\njr $31\nnop\n.end glaze_js_longjmp\n.set reorder\n");
#endif
