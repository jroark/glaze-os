/* Glaze: build only the five supported cartridge mappers. See UPSTREAM.md. */
#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_Mapper.h"
#include "K6502.h"
BYTE DRAM[DRAM_SIZE];
struct MapperTable_tag MapperTable[] = {
 {0, Map0_Init}, {1, Map1_Init}, {2, Map2_Init},
 {3, Map3_Init}, {4, Map4_Init}, {-1, 0}
};
#include "mapper/InfoNES_Mapper_000.c"
#include "mapper/InfoNES_Mapper_001.c"
#include "mapper/InfoNES_Mapper_002.c"
#include "mapper/InfoNES_Mapper_003.c"
#include "mapper/InfoNES_Mapper_004.c"
