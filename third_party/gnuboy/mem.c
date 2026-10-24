

#include <stdlib.h>

#include "defs.h"
#include "hw.h"
#include "regs.h"
#include "mem.h"
#include "rtc.h"
#include "lcd.h"
#include "lcdc.h"
#include "sound.h"

struct mbc mbc;
struct rom rom;
struct ram ram;
struct rom bootrom;

/*
 * In order to make reads and writes efficient, we keep tables
 * (indexed by the high nibble of the address) specifying which
 * regions can be read/written without a function call. For such
 * ranges, the pointer in the map table points to the base of the
 * region in host system memory. For ranges that require special
 * processing, the pointer is NULL.
 *
 * mem_updatemap is called whenever bank changes or other operations
 * make the old maps potentially invalid.
 */

void mem_mapbootrom() {
	if (!bootrom.bank) return;
	mbc.rmap[0x0] = bootrom.bank[0];
}

/* Page-local pointers remain inside their backing allocations. */
void mem_updatemap(void)
{
    int n = R_SVBK & 7;
    if (!n) n=1;
    mbc.bank0 = mbc.type == MBC_MBC1 && mbc.model ? (mbc.hi << 5) : 0;
    if (mbc.type == MBC_MBC1) {
        mbc.rombank = (mbc.hi << 5) | mbc.lo;
        mbc.rambank = mbc.model ? mbc.hi : 0;
    }
    mbc.bank0 &= mbc.romsize-1;
    int bank=mbc.rombank & (mbc.romsize-1);
    int rb=mbc.ramsize ? mbc.rambank & (mbc.ramsize-1) : 0;
    for (int i=0;i<16;i++) mbc.rmap[i]=mbc.wmap[i]=NULL;
    for (int i=0;i<4;i++) {
        mbc.rmap[i]=rom.bank[mbc.bank0]+i*4096;
        mbc.rmap[i+4]=rom.bank[bank]+i*4096;
    }
    mbc.rmap[8]=lcd.vbank[R_VBK&1];
    mbc.rmap[9]=lcd.vbank[R_VBK&1]+4096;
    if (mbc.enableram && mbc.ram_bytes >= 8192 &&
        (mbc.type != MBC_MBC3 || rtc.sel <= 3)) {
        mbc.rmap[10]=mbc.wmap[10]=ram.sbank[rb];
        mbc.rmap[11]=mbc.wmap[11]=ram.sbank[rb]+4096;
    }
    mbc.rmap[12]=mbc.wmap[12]=ram.ibank[0];
    mbc.rmap[13]=mbc.wmap[13]=ram.ibank[n];
    mbc.rmap[14]=mbc.wmap[14]=ram.ibank[0];
}

/*
 * ioreg_write handles output to io registers in the FF00-FF7F,FFFF
 * range. It takes the register number (low byte of the address) and a
 * byte value to be written.
 */

void ioreg_write(byte r, byte b)
{
	if (!hw.cgb)
	{
		switch (r)
		{
		case RI_VBK:
		case RI_BCPS:
		case RI_OCPS:
		case RI_BCPD:
		case RI_OCPD:
		case RI_SVBK:
		case RI_KEY1:
		case RI_HDMA1:
		case RI_HDMA2:
		case RI_HDMA3:
		case RI_HDMA4:
		case RI_HDMA5:
			return;
		}
	}

	switch(r)
	{
	case RI_TIMA:
	case RI_TMA:
	case RI_TAC:
	case RI_SCY:
	case RI_SCX:
	case RI_WY:
	case RI_WX:
		REG(r) = b;
		break;
	case RI_BGP:
		if (R_BGP == b) break;
		pal_write_dmg(0, 0, b);
		pal_write_dmg(8, 1, b);
		R_BGP = b;
		break;
	case RI_OBP0:
		if (R_OBP0 == b) break;
		pal_write_dmg(64, 2, b);
		R_OBP0 = b;
		break;
	case RI_OBP1:
		if (R_OBP1 == b) break;
		pal_write_dmg(72, 3, b);
		R_OBP1 = b;
		break;
	case RI_IF:
	case RI_IE:
		REG(r) = b & 0x1F;
		break;
	case RI_P1:
		REG(r) = b;
		pad_refresh();
		break;
	case RI_SC:
		/* FIXME - this is a hack for stupid roms that probe serial */
		if ((b & 0x81) == 0x81)
		{
			extern void gb_serial(byte value);
            gb_serial(R_SB);
            R_SB = 0xff;
			hw_interrupt(IF_SERIAL, IF_SERIAL);
			hw_interrupt(0, IF_SERIAL);
		}
		R_SC = b; /* & 0x7f; */
		break;
	case RI_SB:
		REG(r) = b;
		break;
	case RI_DIV:
		REG(r) = 0;
		break;
	case RI_LCDC:
		lcdc_change(b);
		break;
	case RI_STAT:
		stat_write(b);
		break;
	case RI_LYC:
		REG(r) = b;
		stat_trigger();
		break;
	case RI_VBK:
		REG(r) = b | 0xFE;
		mem_updatemap();
		break;
	case RI_BCPS:
		R_BCPS = b & 0xBF;
		R_BCPD = lcd.pal[b & 0x3F];
		break;
	case RI_OCPS:
		R_OCPS = b & 0xBF;
		R_OCPD = lcd.pal[64 + (b & 0x3F)];
		break;
	case RI_BCPD:
		R_BCPD = b;
		pal_write(R_BCPS & 0x3F, b);
		if (R_BCPS & 0x80) R_BCPS = (R_BCPS+1) & 0xBF;
		break;
	case RI_OCPD:
		R_OCPD = b;
		pal_write(64 + (R_OCPS & 0x3F), b);
		if (R_OCPS & 0x80) R_OCPS = (R_OCPS+1) & 0xBF;
		break;
	case RI_SVBK:
		REG(r) = b & 0x07;
		mem_updatemap();
		break;
	case RI_BOOT:
		if(!(b&1)) break;
		if(REG(r)&1) break;
		REG(r) = 0xff;
		mem_updatemap();
		break;
	case RI_DMA:
		hw_dma(b);
		break;
	case RI_KEY1:
		REG(r) = (REG(r) & 0x80) | (b & 0x01);
		break;
	case RI_HDMA1:
		REG(r) = b;
		break;
	case RI_HDMA2:
		REG(r) = b & 0xF0;
		break;
	case RI_HDMA3:
		REG(r) = b & 0x1F;
		break;
	case RI_HDMA4:
		REG(r) = b & 0xF0;
		break;
	case RI_HDMA5:
		hw_hdma_cmd(b);
		break;
	}
	switch (r)
	{
	case RI_BGP:
	case RI_OBP0:
	case RI_OBP1:
		/* printf("palette reg %02X write %02X at LY=%02X\n", r, b, R_LY); */
	case RI_HDMA1:
	case RI_HDMA2:
	case RI_HDMA3:
	case RI_HDMA4:
	case RI_HDMA5:
		/* printf("HDMA %d: %02X\n", r - RI_HDMA1 + 1, b); */
		break;
	}
	/* printf("reg %02X => %02X (%02X)\n", r, REG(r), b); */
}


byte ioreg_read(byte r)
{
	switch(r)
	{
	case RI_BOOT:
		return 0xfe | (REG(r) & 1);
	case RI_SC:
		r = R_SC;
		R_SC &= 0x7f;
		return r;
	case RI_P1:
	case RI_SB:
	case RI_DIV:
	case RI_TIMA:
	case RI_TMA:
	case RI_TAC:
	case RI_LCDC:
	case RI_STAT:
	case RI_SCY:
	case RI_SCX:
	case RI_LY:
	case RI_LYC:
	case RI_BGP:
	case RI_OBP0:
	case RI_OBP1:
	case RI_WY:
	case RI_WX:
	case RI_IE:
	case RI_IF:
		return REG(r);
	case RI_VBK:
	case RI_BCPS:
	case RI_OCPS:
	case RI_BCPD:
	case RI_OCPD:
	case RI_SVBK:
	case RI_KEY1:
	case RI_HDMA1:
	case RI_HDMA2:
	case RI_HDMA3:
	case RI_HDMA4:
	case RI_HDMA5:
		if (hw.cgb) return REG(r);
        return 0xff;
	default:
		return 0xff;
	}
}



/*
 * Memory bank controllers typically intercept write attempts to
 * 0000-7FFF, using the address and byte written as instructions to
 * change rom or sram banks, control special hardware, etc.
 *
 * mbc_write takes an address (which should be in the proper range)
 * and a byte value written to the address.
 */

void mbc_write(int a, byte b)
{
    if (mbc.type == MBC_NONE) return;
    if (mbc.type == MBC_MBC2) {
        if (a < 0x4000) {
            if (a&0x100) mbc.rombank=(b&15) ? b&15 : 1;
            else mbc.enableram=(b&15)==10;
        }
    } else if (a < 0x2000) mbc.enableram=(b&15)==10;
    else if (mbc.type == MBC_MBC1) {
        if (a < 0x4000) mbc.lo=(b&31) ? b&31 : 1;
        else if (a < 0x6000) mbc.hi=b&3;
        else mbc.model=b&1;
    } else if (mbc.type == MBC_MBC3) {
        if (a < 0x4000) mbc.rombank=(b&127) ? b&127 : 1;
        else if (a < 0x6000) { rtc.sel=b; mbc.rambank=b&3; }
        else if (rtc.batt) rtc_latch(b);
    } else { /* MBC5, including rumble variants */
        if (a < 0x3000) mbc.rombank=(mbc.rombank&256)|b;
        else if (a < 0x4000) mbc.rombank=(mbc.rombank&255)|((b&1)<<8);
        else if (a < 0x6000) mbc.rambank=b & (mbc.type==MBC_RUMBLE ? 7 : 15);
    }
    mem_updatemap();
}

/*
 * mem_write is the basic write function. Although it should only be
 * called when the write map contains a NULL for the requested address
 * region, it accepts writes to any address.
 */

void mem_write(int a, byte b)
{
	int n;
	a &= 0xffff;
    byte ha = (a>>12) & 0xE;

	/* printf("write to 0x%04X: 0x%02X\n", a, b); */
	switch (ha)
	{
	case 0x0:
	case 0x2:
	case 0x4:
	case 0x6:
		mbc_write(a, b);
		break;
	case 0x8:
		/* if ((R_STAT & 0x03) == 0x03) break; */
		vram_write(a & 0x1FFF, b);
		break;
	case 0xA:
		if (!mbc.enableram) break;
        if (mbc.type == MBC_MBC3 && rtc.sel >= 4) {
            if (rtc.batt && rtc.sel >= 8 && rtc.sel <= 12) rtc_write(b);
            break;
        }
        if (!mbc.ram_bytes) break;
        if (mbc.type == MBC_MBC2) ram.sbank[0][a & 511]=b&15;
        else ram.sbank[mbc.rambank & (mbc.ramsize-1)][a & (mbc.ram_bytes < 8192 ? 2047 : 8191)] = b;
		break;
	case 0xC:
		if ((a & 0xF000) == 0xC000)
		{
			ram.ibank[0][a & 0x0FFF] = b;
			break;
		}
		n = R_SVBK & 0x07;
		ram.ibank[n?n:1][a & 0x0FFF] = b;
		break;
	case 0xE:
		if (a < 0xFE00)
		{
			mem_write(a & 0xDFFF, b);
			break;
		}
		if ((a & 0xFF00) == 0xFE00)
		{
			/* if (R_STAT & 0x02) break; */
			if (a < 0xFEA0) lcd.oam.mem[a & 0xFF] = b;
			break;
		}
		/* return writehi(a & 0xFF, b); */
		if (a >= 0xFF10 && a <= 0xFF3F)
		{
			sound_write(a & 0xFF, b);
			break;
		}
		if ((a & 0xFF80) == 0xFF80 && a != 0xFFFF)
		{
			ram.hi[a & 0xFF] = b;
			break;
		}
		ioreg_write(a & 0xFF, b);
	}
}


/*
 * mem_read is the basic read function...not useful for much anymore
 * with the read map, but it's still necessary for the final messy
 * region.
 */

byte mem_read(int a)
{
	int n;
	a &= 0xffff;
    byte ha = (a>>12) & 0xE;

	/* printf("read %04x\n", a); */
	switch (ha)
	{
	case 0x0:
	case 0x2:
		return rom.bank[mbc.bank0][a];
	case 0x4:
	case 0x6:
		return rom.bank[mbc.rombank & (mbc.romsize-1)][a & 0x3FFF];
	case 0x8:
		/* if ((R_STAT & 0x03) == 0x03) return 0xFF; */
		return lcd.vbank[R_VBK&1][a & 0x1FFF];
	case 0xA:
		if (!mbc.enableram && mbc.type == MBC_HUC3)
			return 0x01;
		if (!mbc.enableram)
			return 0xFF;
        if (mbc.type == MBC_MBC3 && rtc.sel >= 4)
            return rtc.batt && rtc.sel >= 8 && rtc.sel <= 12 ? rtc.regs[rtc.sel-8] : 0xff;
        if (!mbc.ram_bytes) return 0xff;
        if (mbc.type == MBC_MBC2) return 0xf0 | ram.sbank[0][a & 511];
        return ram.sbank[mbc.rambank & (mbc.ramsize-1)][a & (mbc.ram_bytes < 8192 ? 2047 : 8191)];
	case 0xC:
		if ((a & 0xF000) == 0xC000)
			return ram.ibank[0][a & 0x0FFF];
		n = R_SVBK & 0x07;
		return ram.ibank[n?n:1][a & 0x0FFF];
	case 0xE:
		if (a < 0xFE00) return mem_read(a & 0xDFFF);
		if ((a & 0xFF00) == 0xFE00)
		{
			/* if (R_STAT & 0x02) return 0xFF; */
			if (a < 0xFEA0) return lcd.oam.mem[a & 0xFF];
			return 0xFF;
		}
		/* return readhi(a & 0xFF); */
		if (a == 0xFFFF) return REG(0xFF);
		if (a >= 0xFF10 && a <= 0xFF3F)
			return sound_read(a & 0xFF);
		if ((a & 0xFF80) == 0xFF80)
			return ram.hi[a & 0xFF];
		return ioreg_read(a & 0xFF);
	}
	return 0xff; /* not reached */
}

void mbc_reset()
{
	mbc.rombank = mbc.lo = 1;
    mbc.hi = mbc.model = mbc.rambank = 0;
    rtc.sel=rtc.latch=0;
    mbc.enableram = mbc.type == MBC_NONE;
	mem_updatemap();
}

const char* mbc_to_string(int mbc_type) {
	switch (mbc_type) {
	case MBC_NONE:   return "NONE";
	case MBC_MBC1:   return "MBC1";
	case MBC_MBC2:   return "MBC2";
	case MBC_MBC3:   return "MBC3";
	case MBC_MBC5:   return "MBC5";
	case MBC_RUMBLE: return "RUMBL";
	case MBC_HUC1:   return "HUC1";
	case MBC_HUC3:   return "HUC3";
	default: return "UNKN";
	}
}
