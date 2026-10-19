/* User process entry: the kernel starts us with sp at the stack top and
 * a0 = pid. */
	.set	noreorder
	.text
	.globl	_start
_start:
	addiu	$sp, $sp, -32
	jal	__libc_start
	nop
1:	b	1b
	nop
