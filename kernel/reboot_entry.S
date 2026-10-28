/* Position-independent cold-restart stub, copied to PA 1000 before use.
 * Clear stale CE retained RAM and ROM mailboxes (2400/24fc) so the stock
 * boot ROM reloads CE, rather than resuming the RAM overwritten by Glaze.
 * This never touches NAND, CF, or video RAM. */
    .set noreorder
    .set mips3
    .section .text.reboot, "ax"
    .globl reboot_entry
    .globl reboot_entry_end
reboot_entry:
    mtc0 $zero, $12
    nop
    nop
    nop
    /* Flush dirty cached aliases before zeroing through KSEG1. */
    lui $t0, 0x8000
    ori $t1, $t0, 0x8000
1:
    cache 0x01, 0($t0)
    cache 0x00, 0($t0)
    addiu $t0, $t0, 16
    bne $t0, $t1, 1b
    nop
    sync

    /* Zero [2000, 16 MiB), then [0, 1000). The executing page survives. */
    lui $t0, 0xa000
    ori $t0, $t0, 0x2000
    lui $t1, 0xa100
2:
    sw $zero, 0($t0)
    sw $zero, 4($t0)
    sw $zero, 8($t0)
    sw $zero, 12($t0)
    addiu $t0, $t0, 16
    bne $t0, $t1, 2b
    nop
    lui $t0, 0xa000
    ori $t1, $t0, 0x1000
3:
    sw $zero, 0($t0)
    sw $zero, 4($t0)
    sw $zero, 8($t0)
    sw $zero, 12($t0)
    addiu $t0, $t0, 16
    bne $t0, $t1, 3b
    nop
    sync

    /* VR4131 UM 12.2.4: PMUCNT2.SOFTRST, equivalent to the reset switch. */
    lui $t0, 0xaf00
    lhu $t1, 0xc6($t0)
    ori $t1, $t1, 0x10
    sh $t1, 0xc6($t0)
    sync
    /* If the PMU reset is unavailable, use Casio NK's reset sequence
     * (8007a140): KjCMU A0C4=7, A0C8=10. */
    lui $t0, 0xaa00
    ori $t0, $t0, 0xa000
    addiu $t1, $zero, 7
    sw $t1, 0xc4($t0)
    addiu $t1, $zero, 10
    sw $t1, 0xc8($t0)
    sync
4:
    b 4b
    nop
reboot_entry_end:
