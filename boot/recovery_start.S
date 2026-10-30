/* Masked-ROM CF recovery entry. All payloads were loaded by the ROM into
 * staging RAM, away from its low-memory globals. No NAND accesses here.
 * It beeps on entry and again just before jumping to the kernel. */
    .set noreorder
    .set mips2

/* A long low piezo beep (1 kHz, BLG channel 09A0, docs/BUZZER.md), timed
 * by CP0 Count at about 10 MHz. A power-on boot has its LCD off, so this is
 * the only sign that the ROM reached the stub; the kernel's BOOT_BEEP=1
 * checkpoints use short high beeps. */
.macro beep
    li $t0, 0xAA001128          /* shared CMM gate */
    lw $t1, 0($t0)
    ori $t1, $t1, 1
    sw $t1, 0($t0)
    li $t0, 0xAA000984          /* BLG configuration: buzzer bits */
    lw $t1, 0($t0)
    ori $t1, $t1, 0x0202
    sw $t1, 0($t0)
    li $t0, 0xAA0009A4
    li $t1, 1410                /* 1411200 / 1000 - 1 */
    sw $t1, 0($t0)
    li $t1, 705
    sw $t1, 4($t0)
    li $t1, 1
    sw $t1, -4($t0)             /* start */
    li $t3, 4000000             /* 400 ms */
    mfc0 $t1, $9
    nop
101:
    mfc0 $t2, $9
    nop
    subu $t2, $t2, $t1
    sltu $t2, $t2, $t3
    bnez $t2, 101b
    nop
    sw $zero, -4($t0)           /* stop */
    li $t3, 4000000             /* then 400 ms quiet */
    mfc0 $t1, $9
    nop
102:
    mfc0 $t2, $9
    nop
    subu $t2, $t2, $t1
    sltu $t2, $t2, $t3
    bnez $t2, 102b
    nop
.endm

    .text
    .globl _start
_start:
    beep
    /* Unique entry marker on a retained LCD: if the ROM reaches this stub,
     * the upper-right 16 pixels become bright green before any RAM copy.
     * Only with both LCD gates on (glaze/bootinfo.h gz_boot_trace_live):
     * at power-on they are off, and a framebuffer store would hang. */
    li $t0, 0xAA001100
    lw $t1, 0($t0)
    lw $t2, 0xC($t0)
    and $t1, $t1, $t2
    andi $t1, $t1, 1
    beqz $t1, 6f
    nop
    li $t0, 0xAA2001C0
    li $t1, 0x07E007E0
    li $t3, 16
0:
    li $t2, 8
5:
    sw $t1, 0($t0)
    addiu $t0, $t0, 4
    addiu $t2, $t2, -1
    bnez $t2, 5b
    nop
    addiu $t0, $t0, 480
    addiu $t3, $t3, -1
    bnez $t3, 0b
    nop
6:
    mtc0 $zero, $12
    mtc0 $zero, $13
    la $t0, 1f
    lui $t1, 0x2000
    or $t0, $t0, $t1
    jr $t0
    nop
1:
    .set push
    .set mips3
    lui $t0, 0x8000
    ori $t1, $t0, 0x8000
2:
    cache 0x01, 0($t0)
    cache 0x00, 0($t0)
    addiu $t0, $t0, 16
    bne $t0, $t1, 2b
    nop
    sync
    .set pop
    li $t0, 0xA0300000
    li $t1, RECOVERY_LOAD
    lui $t2, 0x2000
    or $t1, $t1, $t2
    li $t2, RECOVERY_WORDS
3:
    lw $t3, 0($t0)
    sw $t3, 0($t1)
    addiu $t0, $t0, 4
    addiu $t1, $t1, 4
    addiu $t2, $t2, -1
    bnez $t2, 3b
    nop
    /* The 32-byte bootinfo follows the staged, word-padded kernel. */
    li $t1, 0xA000F000
    li $t2, 8
4:
    lw $t3, 0($t0)
    sw $t3, 0($t1)
    addiu $t0, $t0, 4
    addiu $t1, $t1, 4
    addiu $t2, $t2, -1
    bnez $t2, 4b
    nop
    beep
    li $a0, 0x8000F000
    move $a1, $zero
    move $a2, $zero
    move $a3, $zero
    li $t0, RECOVERY_ENTRY
    lui $t1, 0x2000
    or $t0, $t0, $t1
    jr $t0
    nop
