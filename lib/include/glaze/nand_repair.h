/* Explicit raw repair session; ordinary NAND read-only boots cannot use it. */
#ifndef GLAZE_NAND_REPAIR_H
#define GLAZE_NAND_REPAIR_H
#define GZ_NAND_REPAIR_BEGIN 0u   /* bad[128], changed[128] */
#define GZ_NAND_REPAIR_BLOCK 1u   /* block, source CRC32, target CRC32 */
#define GZ_NAND_REPAIR_DATA 2u    /* first page within block, 1..7 raw pages */
#define GZ_NAND_REPAIR_COMMIT 3u  /* complete block staged before erase */
#define GZ_NAND_REPAIR_ABORT 4u
#define GZ_NAND_REPAIR_END 5u
#define GZ_NAND_REPAIR_AVAILABLE 6u /* no mutation */
#define GZ_NAND_REPAIR_BLOCK_BYTES (32u*528u)
#endif
