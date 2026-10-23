InfoNES by the InfoNES Project, based on pNesX, NesterJ and DarcNES.
Source: https://github.com/jay-kumogata/InfoNES
Revision: fe3295c0a86bf5bbecc22aeab3b3af11f6f47908
License: upstream Apache-2.0, reproduced in LICENSE. Original file credits retained.

Glaze copies only the CPU, PPU, integer APU and mappers 0–4. The .cpp files
contain C and are compiled as C, with normalized line endings/trailing whitespace and .c names.
Local changes: uint32_t DWORD for 32/64-bit host consistency; mapper table limited
to 0–4; corrected out-of-bounds APU_Reg[0x4015] to [0x15]; palette indexes masked
to 6 bits; bounded APU event queue. The system callbacks and strict iNES loader
are in packages/nes. This is a scanline emulator, not a cycle-accurate core;
compatibility with timing-sensitive games is limited.

Additional local fixes found by the sanitizer/CPU oracle: C enum spelling in
MMC1; sequenced PC operand fetches; signed APU envelope/sweep phase counters;
22050 Hz APU setting; branch and absolute-X NOP page-crossing cycle penalties;
stable unofficial SLO/RLA/SRE/RRA/DCP/ISC/LAX/SAX and alternate SBC instructions.
The platform packs RGB555 so bit 15 remains the core's sprite-priority marker,
and converts to Glaze RGB565 only at presentation.
