/* Glaze system callbacks for InfoNES, independent of Glaze's UI and filesystem. */
#include "core.h"
#include "../../third_party/infones/InfoNES.h"
#include "../../third_party/infones/InfoNES_System.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct nes_cart nes_cartridge;
WORD NesPalette[64];
static int previous_sample, filtered_sample;
static const uint32_t palette[64] = {
 0x666666,0x002a88,0x1412a7,0x3b00a4,0x5c007e,0x6e0040,0x6c0600,0x561d00,
 0x333500,0x0b4800,0x005200,0x004f08,0x00404d,0,0,0,
 0xadadad,0x155fd9,0x4240ff,0x7527fe,0xa01acc,0xb71e7b,0xb53120,0x994e00,
 0x6b6d00,0x388700,0x0c9300,0x008f32,0x007c8d,0,0,0,
 0xfffeff,0x64b0ff,0x9290ff,0xc676ff,0xf36aff,0xfe6ecc,0xfe8170,0xea9e22,
 0xbcbe00,0x88d800,0x5ce430,0x45e082,0x48cdde,0x4f4f4f,0,0,
 0xfffeff,0xc0dfff,0xd3d2ff,0xe8c8ff,0xfbc2ff,0xfec4ea,0xfeccc5,0xf7d8a5,
 0xe4e594,0xcfef96,0xbdf4ab,0xb3f3cc,0xb5ebf2,0xb8b8b8,0,0
};

void InfoNES_ReleaseRom(void)
{
    free(ROM); free(VROM); ROM = VROM = NULL;
}

void nes_close(void)
{
    InfoNES_Fin();
    memset(&nes_cartridge, 0, sizeof nes_cartridge);
}

const char *nes_open(const uint8_t *data, size_t size)
{
    struct nes_cart c;
    const char *error = nes_validate(data, size, &c);
    if (error) return error;
    uint8_t *prg = malloc(c.prg_size);
    uint8_t *chr = c.chr_size ? malloc(c.chr_size) : NULL;
    if (!prg || (c.chr_size && !chr)) {
        free(prg); free(chr); return "Not enough memory for cartridge";
    }
    memcpy(prg, data + c.prg_offset, c.prg_size);
    if (chr) memcpy(chr, data + c.chr_offset, c.chr_size);
    nes_close();
    ROM = prg; VROM = chr;
    nes_cartridge = c;
    /* Adapt parsed metadata to InfoNES's legacy configuration. Raw NES 2.0
     * size/region bytes are not legacy bank counts or reserved bytes.
     * ROM payload and the caller's file remain byte-for-byte unchanged. */
    memset(&NesHeader, 0, sizeof NesHeader);
    memcpy(NesHeader.byID, "NES\032", 4);
    NesHeader.byRomSize = c.prg_size / 16384;
    NesHeader.byVRomSize = c.chr_size / 8192;
    NesHeader.byInfo1 = (data[6] & 15) | ((c.mapper & 15) << 4);
    NesHeader.byInfo2 = c.mapper & 0xf0;
    memset(SRAM, 0, SRAM_SIZE);
    if (c.trainer) memcpy(SRAM + 0x1000, data + 16, 512);
    for (int i = 0; i < 64; i++) {
        uint32_t rgb = palette[i];
        NesPalette[i] = ((rgb >> 9) & 0x7c00) | ((rgb >> 6) & 0x3e0) | ((rgb >> 3) & 31);
    }
    InfoNES_Init();
    if (InfoNES_Reset() < 0) { nes_close(); return "Cartridge reset failed"; }
    FrameSkip = 1; /* Render 30 times/s; CPU, input and APU still run every frame. */
    return NULL;
}

void nes_reset(void) { if (ROM) { InfoNES_Reset(); FrameSkip = 1; } }
void nes_run(void) { if (ROM) InfoNES_Cycle(); }
uint8_t *nes_sram(void) { return SRAM; }
int InfoNES_Menu(void) { return -1; }
int InfoNES_ReadRom(const char *name) { (void)name; return -1; }
void *InfoNES_MemoryCopy(void *d, const void *s, int n) { return memcpy(d,s,(size_t)n); }
void *InfoNES_MemorySet(void *d, int c, int n) { return memset(d,c,(size_t)n); }
void InfoNES_LoadFrame(void) { nes_video(WorkFrame); }
void InfoNES_Wait(void) {}
void InfoNES_PadState(DWORD *p1, DWORD *p2, DWORD *system)
{
    unsigned p = nes_poll();
    *p1 = p & 255; *p2 = 0; *system = (p & 256) ? PAD_SYS_QUIT : 0;
}
void InfoNES_DebugPrint(char *s) { nes_log(s); }
void InfoNES_MessageBox(char *fmt, ...)
{
    char s[160]; va_list ap; va_start(ap,fmt); vsnprintf(s,sizeof s,fmt,ap); va_end(ap); nes_log(s);
}
void InfoNES_SoundInit(void) { nes_audio_close(); }
int InfoNES_SoundOpen(int samples, int rate) { (void)samples; previous_sample=filtered_sample=0; nes_audio_open((unsigned)rate); return 1; }
void InfoNES_SoundClose(void) { nes_audio_close(); }
void InfoNES_SoundOutput(int n, BYTE *a, BYTE *b, BYTE *c, BYTE *d, BYTE *e)
{
    int16_t pcm[735]; /* Largest supported 44100/60 batch. */
    if (n < 0 || n > 735) return;
    /* Integer DC blocker; avoid an audible DC step when the APU starts. */
    for (int i = 0; i < n; i++) {
        int sample = ((int)a[i] + b[i] + c[i] + d[i] + e[i]) * 48;
        filtered_sample = sample - previous_sample + (filtered_sample * 255) / 256;
        previous_sample = sample;
        int value = filtered_sample;
        pcm[i] = value < -32768 ? -32768 : value > 32767 ? 32767 : value;
    }
    nes_audio(pcm,(unsigned)n);
}
