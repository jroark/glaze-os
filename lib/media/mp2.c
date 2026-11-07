/*
 * MPEG-1 and MPEG-2 (LSF) Layer II audio decoding (see glaze/media.h).
 *
 * A fixed-point port of pl_mpeg's decoder (third_party/pl_mpeg, MIT,
 * itself based on Martin J. Fiedler's kjmp2). Bit allocation and scale
 * factors are pl_mpeg's integer code, dequantization follows the standard's
 * step sizes (see read_samples); the synthesis
 * filterbank, which pl_mpeg does in float, runs here on 32-bit samples
 * (subband samples scaled by 16) with Q27 IDCT constants and 64-bit
 * window sums, so it needs no soft-float.
 */

#include <glaze/media.h>
#include <string.h>

/* ---- tables (pl_mpeg) ------------------------------------------------------ */

static const uint16_t sample_rate[] = {
    44100, 48000, 32000, 0,     /* MPEG-1 */
    22050, 24000, 16000, 0,     /* MPEG-2 LSF */
};

static const uint16_t bit_rate[] = {
    32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384,     /* MPEG-1 */
    8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160,          /* MPEG-2 */
};

static const int32_t scalefactor_base[] = { 0x02000000, 0x01965FEA, 0x01428A30 };

/* Quantizer lookup, step 1: channel mode, bitrate index -> bitrate class. */
static const uint8_t quant_lut_step_1[2][16] = {
    /* 32, 48, 56, 64, 80, 96,112,128,160,192,224,256,320,384 <- bitrate */
    { 0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2 },     /* mono */
    /* 16, 24, 28, 32, 40, 48, 56, 64, 80, 96,112,128,160,192 <- bitrate / chan */
    { 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 2 },     /* stereo */
};

/* Step 2: bitrate class, sample rate -> B2 table (upper bits), sblimit. */
#define QUANT_TAB_A (27 | 64)   /* Table 3-B.2a: high-rate, sblimit = 27 */
#define QUANT_TAB_B (30 | 64)   /* Table 3-B.2b: high-rate, sblimit = 30 */
#define QUANT_TAB_C 8           /* Table 3-B.2c: low-rate, sblimit = 8 */
#define QUANT_TAB_D 12          /* Table 3-B.2d: low-rate, sblimit = 12 */
#define QUANT_TAB_LSF (30 | 128) /* ISO 13818-3 Table B.1, sblimit = 30 */

static const uint8_t quant_lut_step_2[3][3] = {
    /* 44.1 kHz,  48 kHz,     32 kHz */
    { QUANT_TAB_C, QUANT_TAB_C, QUANT_TAB_D },  /* 32 - 48 kbit/s/ch */
    { QUANT_TAB_A, QUANT_TAB_A, QUANT_TAB_A },  /* 56 - 80 kbit/s/ch */
    { QUANT_TAB_B, QUANT_TAB_A, QUANT_TAB_B },  /* 96+ kbit/s/ch */
};

/* Step 3: B2 table, subband -> nbal (upper 4 bits), row index. */
static const uint8_t quant_lut_step_3[3][32] = {
    /* Low-rate table (3-B.2c and 3-B.2d) */
    { 0x44, 0x44, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34 },
    /* High-rate table (3-B.2a and 3-B.2b) */
    { 0x43, 0x43, 0x43, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42,
      0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31,
      0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 },
    /* MPEG-2 LSR table (B.2 in ISO 13818-3) */
    { 0x45, 0x45, 0x45, 0x45, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34,
      0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24,
      0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24 },
};

/* Step 4: table row, allocation value -> quantizer (1-based, 0 = none). */
static const uint8_t quant_lut_step_4[6][16] = {
    { 0, 1, 2, 17 },
    { 0, 1, 2, 3, 4, 5, 6, 17 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 17 },
    { 0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17 },
    { 0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
};

struct quantizer { uint16_t levels; uint8_t group, bits; };

static const struct quantizer quant_tab[] = {
    { 3, 1, 5 }, { 5, 1, 7 }, { 7, 0, 3 }, { 9, 1, 10 }, { 15, 0, 4 }, { 31, 0, 5 },
    { 63, 0, 6 }, { 127, 0, 7 }, { 255, 0, 8 }, { 511, 0, 9 }, { 1023, 0, 10 },
    { 2047, 0, 11 }, { 4095, 0, 12 }, { 8191, 0, 13 }, { 16383, 0, 14 },
    { 32767, 0, 15 }, { 65535, 0, 16 },
};

/* The synthesis window, doubled so that it is integral. */
static const int32_t window2[512] = {
    0, -1, -1, -1, -1, -1, -1, -2, -2, -2,
    -2, -3, -3, -4, -4, -5, -5, -6, -7, -7,
    -8, -9, -10, -11, -13, -14, -16, -17, -19, -21,
    -24, -26, -29, -31, -35, -38, -41, -45, -49, -53,
    -58, -63, -68, -73, -79, -85, -91, -97, -104, -111,
    -117, -125, -132, -139, -147, -154, -161, -169, -176, -183,
    -190, -196, -202, -208, 213, 218, 222, 225, 227, 228,
    228, 227, 224, 221, 215, 208, 200, 189, 177, 163,
    146, 127, 106, 83, 57, 29, -2, -36, -72, -111,
    -153, -197, -244, -294, -347, -401, -459, -519, -581, -645,
    -711, -779, -848, -919, -991, -1064, -1137, -1210, -1283, -1356,
    -1428, -1498, -1567, -1634, -1698, -1759, -1817, -1870, -1919, -1962,
    -2001, -2032, -2057, -2075, -2085, -2087, -2080, -2063, 2037, 2000,
    1952, 1893, 1822, 1739, 1644, 1535, 1414, 1280, 1131, 970,
    794, 605, 402, 185, -45, -288, -545, -814, -1095, -1388,
    -1692, -2006, -2330, -2663, -3004, -3351, -3705, -4063, -4425, -4788,
    -5153, -5517, -5879, -6237, -6589, -6935, -7271, -7597, -7910, -8209,
    -8491, -8755, -8998, -9219, -9416, -9585, -9727, -9838, -9916, -9959,
    -9966, -9935, -9863, -9750, -9592, -9389, -9139, -8840, -8492, -8092,
    -7640, -7134, 6574, 5959, 5288, 4561, 3776, 2935, 2037, 1082,
    70, -998, -2122, -3300, -4533, -5818, -7154, -8540, -9975, -11455,
    -12980, -14548, -16155, -17799, -19478, -21189, -22929, -24694, -26482, -28289,
    -30112, -31947, -33791, -35640, -37489, -39336, -41176, -43006, -44821, -46617,
    -48390, -50137, -51853, -53534, -55178, -56778, -58333, -59838, -61289, -62684,
    -64019, -65290, -66494, -67629, -68692, -69679, -70590, -71420, -72169, -72835,
    -73415, -73908, -74313, -74630, -74856, -74992, 75038, 74992, 74856, 74630,
    74313, 73908, 73415, 72835, 72169, 71420, 70590, 69679, 68692, 67629,
    66494, 65290, 64019, 62684, 61289, 59838, 58333, 56778, 55178, 53534,
    51853, 50137, 48390, 46617, 44821, 43006, 41176, 39336, 37489, 35640,
    33791, 31947, 30112, 28289, 26482, 24694, 22929, 21189, 19478, 17799,
    16155, 14548, 12980, 11455, 9975, 8540, 7154, 5818, 4533, 3300,
    2122, 998, -70, -1082, -2037, -2935, -3776, -4561, -5288, -5959,
    6574, 7134, 7640, 8092, 8492, 8840, 9139, 9389, 9592, 9750,
    9863, 9935, 9966, 9959, 9916, 9838, 9727, 9585, 9416, 9219,
    8998, 8755, 8491, 8209, 7910, 7597, 7271, 6935, 6589, 6237,
    5879, 5517, 5153, 4788, 4425, 4063, 3705, 3351, 3004, 2663,
    2330, 2006, 1692, 1388, 1095, 814, 545, 288, 45, -185,
    -402, -605, -794, -970, -1131, -1280, -1414, -1535, -1644, -1739,
    -1822, -1893, -1952, -2000, 2037, 2063, 2080, 2087, 2085, 2075,
    2057, 2032, 2001, 1962, 1919, 1870, 1817, 1759, 1698, 1634,
    1567, 1498, 1428, 1356, 1283, 1210, 1137, 1064, 991, 919,
    848, 779, 711, 645, 581, 519, 459, 401, 347, 294,
    244, 197, 153, 111, 72, 36, 2, -29, -57, -83,
    -106, -127, -146, -163, -177, -189, -200, -208, -215, -221,
    -224, -227, -228, -228, -227, -225, -222, -218, 213, 208,
    202, 196, 190, 183, 176, 169, 161, 154, 147, 139,
    132, 125, 117, 111, 104, 97, 91, 85, 79, 73,
    68, 63, 58, 53, 49, 45, 41, 38, 35, 31,
    29, 26, 24, 21, 19, 17, 16, 14, 13, 11,
    10, 9, 8, 7, 7, 6, 5, 5, 4, 4,
    3, 3, 2, 2, 2, 2, 1, 1, 1, 1,
    1, 1,
};

#define MULC(a, c) ((int32_t)(((int64_t)(a) * (c)) >> 27))

/* pl_mpeg's 32-point IDCT (plm_audio_idct36), constants in Q27. Built for
 * size: at -O2 GCC turns each constant multiply into shifts and adds,
 * 10 KiB of them. */
#if defined(__GNUC__) && !defined(__clang__)
#define FOR_SIZE __attribute__((noinline, optimize("Os")))
#else
#define FOR_SIZE __attribute__((noinline))
#endif
static FOR_SIZE void idct32(const int32_t s[32][3], int ss, int32_t *d, int dp)
{
    int32_t t01, t02, t03, t04, t05, t06, t07, t08, t09, t10, t11, t12,
        t13, t14, t15, t16, t17, t18, t19, t20, t21, t22, t23, t24,
        t25, t26, t27, t28, t29, t30, t31, t32, t33;


    t01 = (s[0][ss] + s[31][ss]); t02 = MULC((s[0][ss] - s[31][ss]), 67189797);
    t03 = (s[1][ss] + s[30][ss]); t04 = MULC((s[1][ss] - s[30][ss]), 67843164);
    t05 = (s[2][ss] + s[29][ss]); t06 = MULC((s[2][ss] - s[29][ss]), 69182167);
    t07 = (s[3][ss] + s[28][ss]); t08 = MULC((s[3][ss] - s[28][ss]), 71275330);
    t09 = (s[4][ss] + s[27][ss]); t10 = MULC((s[4][ss] - s[27][ss]), 74236348);
    t11 = (s[5][ss] + s[26][ss]); t12 = MULC((s[5][ss] - s[26][ss]), 78240207);
    t13 = (s[6][ss] + s[25][ss]); t14 = MULC((s[6][ss] - s[25][ss]), 83551089);
    t15 = (s[7][ss] + s[24][ss]); t16 = MULC((s[7][ss] - s[24][ss]), 90571242);
    t17 = (s[8][ss] + s[23][ss]); t18 = MULC((s[8][ss] - s[23][ss]), 99929967);
    t19 = (s[9][ss] + s[22][ss]); t20 = MULC((s[9][ss] - s[22][ss]), 112655602);
    t21 = (s[10][ss] + s[21][ss]); t22 = MULC((s[10][ss] - s[21][ss]), 130535899);
    t23 = (s[11][ss] + s[20][ss]); t24 = MULC((s[11][ss] - s[20][ss]), 156959571);
    t25 = (s[12][ss] + s[19][ss]); t26 = MULC((s[12][ss] - s[19][ss]), 199201203);
    t27 = (s[13][ss] + s[18][ss]); t28 = MULC((s[13][ss] - s[18][ss]), 276190692);
    t29 = (s[14][ss] + s[17][ss]); t30 = MULC((s[14][ss] - s[17][ss]), 457361460);
    t31 = (s[15][ss] + s[16][ss]); t32 = MULC((s[15][ss] - s[16][ss]), 1367679739);

    t33 = t01 + t31; t31 = MULC((t01 - t31), 67433575);
    t01 = t03 + t29; t29 = MULC((t03 - t29), 70128577);
    t03 = t05 + t27; t27 = MULC((t05 - t27), 76093940);
    t05 = t07 + t25; t25 = MULC((t07 - t25), 86814950);
    t07 = t09 + t23; t23 = MULC((t09 - t23), 105784323);
    t09 = t11 + t21; t21 = MULC((t11 - t21), 142361749);
    t11 = t13 + t19; t19 = MULC((t13 - t19), 231182936);
    t13 = t15 + t17; t17 = MULC((t15 - t17), 684664578);
    t15 = t33 + t13; t13 = MULC((t33 - t13), 68423604);
    t33 = t01 + t11; t01 = MULC((t01 - t11), 80711144);
    t11 = t03 + t09; t09 = MULC((t03 - t09), 120792764);
    t03 = t05 + t07; t07 = MULC((t05 - t07), 343988688);
    t05 = t15 + t03; t15 = MULC((t15 - t03), 72638111);
    t03 = t33 + t11; t11 = MULC((t33 - t11), 175363913);
    t33 = t05 + t03; t05 = MULC((t05 - t03), 94906266);
    t03 = t15 + t11; t15 = MULC((t15 - t11), 94906266);
    t03 += t15;
    t11 = t13 + t07; t13 = MULC((t13 - t07), 72638111);
    t07 = t01 + t09; t09 = MULC((t01 - t09), 175363913);
    t01 = t11 + t07; t07 = MULC((t11 - t07), 94906266);
    t11 = t13 + t09; t13 = MULC((t13 - t09), 94906266);
    t11 += t13; t01 += t11;
    t11 += t07; t07 += t13;
    t09 = t31 + t17; t31 = MULC((t31 - t17), 68423604);
    t17 = t29 + t19; t29 = MULC((t29 - t19), 80711144);
    t19 = t27 + t21; t21 = MULC((t27 - t21), 120792764);
    t27 = t25 + t23; t23 = MULC((t25 - t23), 343988688);
    t25 = t09 + t27; t09 = MULC((t09 - t27), 72638111);
    t27 = t17 + t19; t19 = MULC((t17 - t19), 175363913);
    t17 = t25 + t27; t27 = MULC((t25 - t27), 94906266);
    t25 = t09 + t19; t19 = MULC((t09 - t19), 94906266);
    t25 += t19;
    t09 = t31 + t23; t31 = MULC((t31 - t23), 72638111);
    t23 = t29 + t21; t21 = MULC((t29 - t21), 175363913);
    t29 = t09 + t23; t23 = MULC((t09 - t23), 94906266);
    t09 = t31 + t21; t31 = MULC((t31 - t21), 94906266);
    t09 += t31;    t29 += t09;    t09 += t23;    t23 += t31;
    t17 += t29;    t29 += t25;    t25 += t09;    t09 += t27;
    t27 += t23;    t23 += t19; t19 += t31;
    t21 = t02 + t32; t02 = MULC((t02 - t32), 67433575);
    t32 = t04 + t30; t04 = MULC((t04 - t30), 70128577);
    t30 = t06 + t28; t28 = MULC((t06 - t28), 76093940);
    t06 = t08 + t26; t08 = MULC((t08 - t26), 86814950);
    t26 = t10 + t24; t10 = MULC((t10 - t24), 105784323);
    t24 = t12 + t22; t22 = MULC((t12 - t22), 142361749);
    t12 = t14 + t20; t20 = MULC((t14 - t20), 231182936);
    t14 = t16 + t18; t16 = MULC((t16 - t18), 684664578);
    t18 = t21 + t14; t14 = MULC((t21 - t14), 68423604);
    t21 = t32 + t12; t32 = MULC((t32 - t12), 80711144);
    t12 = t30 + t24; t24 = MULC((t30 - t24), 120792764);
    t30 = t06 + t26; t26 = MULC((t06 - t26), 343988688);
    t06 = t18 + t30; t18 = MULC((t18 - t30), 72638111);
    t30 = t21 + t12; t12 = MULC((t21 - t12), 175363913);
    t21 = t06 + t30; t30 = MULC((t06 - t30), 94906266);
    t06 = t18 + t12; t12 = MULC((t18 - t12), 94906266);
    t06 += t12;
    t18 = t14 + t26; t26 = MULC((t14 - t26), 72638111);
    t14 = t32 + t24; t24 = MULC((t32 - t24), 175363913);
    t32 = t18 + t14; t14 = MULC((t18 - t14), 94906266);
    t18 = t26 + t24; t24 = MULC((t26 - t24), 94906266);
    t18 += t24; t32 += t18;
    t18 += t14; t26 = t14 + t24;
    t14 = t02 + t16; t02 = MULC((t02 - t16), 68423604);
    t16 = t04 + t20; t04 = MULC((t04 - t20), 80711144);
    t20 = t28 + t22; t22 = MULC((t28 - t22), 120792764);
    t28 = t08 + t10; t10 = MULC((t08 - t10), 343988688);
    t08 = t14 + t28; t14 = MULC((t14 - t28), 72638111);
    t28 = t16 + t20; t20 = MULC((t16 - t20), 175363913);
    t16 = t08 + t28; t28 = MULC((t08 - t28), 94906266);
    t08 = t14 + t20; t20 = MULC((t14 - t20), 94906266);
    t08 += t20;
    t14 = t02 + t10; t02 = MULC((t02 - t10), 72638111);
    t10 = t04 + t22; t22 = MULC((t04 - t22), 175363913);
    t04 = t14 + t10; t10 = MULC((t14 - t10), 94906266);
    t14 = t02 + t22; t02 = MULC((t02 - t22), 94906266);
    t14 += t02;    t04 += t14;    t14 += t10;    t10 += t02;
    t16 += t04;    t04 += t08;    t08 += t14;    t14 += t28;
    t28 += t10;    t10 += t20;    t20 += t02;    t21 += t16;
    t16 += t32;    t32 += t04;    t04 += t06;    t06 += t08;
    t08 += t18;    t18 += t14;    t14 += t30;    t30 += t28;
    t28 += t26;    t26 += t10;    t10 += t12;    t12 += t20;
    t20 += t24;    t24 += t02;

    d[dp + 48] = -t33;
    d[dp + 49] = d[dp + 47] = -t21;
    d[dp + 50] = d[dp + 46] = -t17;
    d[dp + 51] = d[dp + 45] = -t16;
    d[dp + 52] = d[dp + 44] = -t01;
    d[dp + 53] = d[dp + 43] = -t32;
    d[dp + 54] = d[dp + 42] = -t29;
    d[dp + 55] = d[dp + 41] = -t04;
    d[dp + 56] = d[dp + 40] = -t03;
    d[dp + 57] = d[dp + 39] = -t06;
    d[dp + 58] = d[dp + 38] = -t25;
    d[dp + 59] = d[dp + 37] = -t08;
    d[dp + 60] = d[dp + 36] = -t11;
    d[dp + 61] = d[dp + 35] = -t18;
    d[dp + 62] = d[dp + 34] = -t09;
    d[dp + 63] = d[dp + 33] = -t14;
    d[dp + 32] = -t05;
    d[dp + 0] = t05; d[dp + 31] = -t30;
    d[dp + 1] = t30; d[dp + 30] = -t27;
    d[dp + 2] = t27; d[dp + 29] = -t28;
    d[dp + 3] = t28; d[dp + 28] = -t07;
    d[dp + 4] = t07; d[dp + 27] = -t26;
    d[dp + 5] = t26; d[dp + 26] = -t23;
    d[dp + 6] = t23; d[dp + 25] = -t10;
    d[dp + 7] = t10; d[dp + 24] = -t15;
    d[dp + 8] = t15; d[dp + 23] = -t12;
    d[dp + 9] = t12; d[dp + 22] = -t19;
    d[dp + 10] = t19; d[dp + 21] = -t20;
    d[dp + 11] = t20; d[dp + 20] = -t13;
    d[dp + 12] = t13; d[dp + 19] = -t24;
    d[dp + 13] = t24; d[dp + 18] = -t31;
    d[dp + 14] = t31; d[dp + 17] = -t02;
    d[dp + 15] = t02; d[dp + 16] = 0;
}

/* ---- bits ---------------------------------------------------------------- */

struct bits {
    const uint8_t *p;
    uint32_t pos, end;          /* in bits */
};

/* Reads n (<= 16) bits; past the end reads zeros. */
static uint32_t rd(struct bits *b, int n)
{
    uint32_t v = 0;

    for (int i = 0; i < n; i++, b->pos++)
        v = v << 1 | (b->pos < b->end ? (uint32_t)(b->p[b->pos >> 3] >> (7 - (b->pos & 7)) & 1) : 0);
    return v;
}

/* ---- frames ---------------------------------------------------------------- */

int media_mp2_header(const uint8_t *p, uint32_t len, struct media_mp2_info *h)
{
    if (len < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0)
        return -1;
    int version = p[1] >> 3 & 3, layer = p[1] >> 1 & 3;
    int br = p[2] >> 4, sr = p[2] >> 2 & 3, pad = p[2] >> 1 & 1, mode = p[3] >> 6;

    if ((version != 3 && version != 2) || layer != 2 || br == 0 || br == 15 || sr == 3)
        return -1;
    int lsf = version == 2;
    memset(h, 0, sizeof(*h));
    h->lsf = (uint8_t)lsf;
    h->bitrate_index = (uint8_t)(br - 1);
    h->rate_index = (uint8_t)sr;
    h->mode = (uint8_t)mode;
    h->mode_ext = (uint8_t)(p[3] >> 4 & 3);
    h->crc = !(p[1] & 1);
    h->rate = sample_rate[sr + 4 * lsf];
    h->bitrate = bit_rate[br - 1 + 14 * lsf];
    h->channels = mode == 3 ? 1 : 2;
    h->frame_len = (uint16_t)(144000u * h->bitrate / h->rate + (uint32_t)pad);
    return 0;
}

static const struct quantizer *read_allocation(struct bits *b, int sb, int tab3)
{
    int tab4 = quant_lut_step_3[tab3][sb];
    int qtab = quant_lut_step_4[tab4 & 15][rd(b, tab4 >> 4)];
    return qtab ? &quant_tab[qtab - 1] : NULL;
}

static void read_samples(struct media_mp2 *d, struct bits *b, int ch, int sb, int part)
{
    const struct quantizer *q = (const struct quantizer *)d->allocation[ch][sb];
    int32_t sf = d->scale_factor[ch][sb][part];
    int32_t *sample = d->sample[ch][sb];

    if (!q) {
        sample[0] = sample[1] = sample[2] = 0;
        return;
    }
    if (sf == 63) {
        sf = 0;
    } else {
        int shift = sf / 3;
        sf = (scalefactor_base[sf % 3] + ((1 << shift) >> 1)) >> shift;
    }
    int32_t adj = q->levels;
    if (q->group) {
        int32_t val = (int32_t)rd(b, q->bits);
        sample[0] = val % adj;
        val /= adj;
        sample[1] = val % adj;
        sample[2] = val / adj;
    } else {
        sample[0] = (int32_t)rd(b, q->bits);
        sample[1] = (int32_t)rd(b, q->bits);
        sample[2] = (int32_t)rd(b, q->bits);
    }
    /* ISO 11172-3 2.4.3.3.4: code c is (c - (levels - 1) / 2) * 2 / levels,
     * here in units of 1/32768 (C and D of Table 3-B.4 folded together).
     * pl_mpeg and kjmp2 use 65536 / (levels + 1), up to 2.5 dB low in
     * coarsely quantized subbands. */
    int32_t scale = (65536 + adj / 2) / adj;
    adj = (adj - 1) >> 1;
    for (int i = 0; i < 3; i++) {
        int32_t val = (adj - sample[i]) * scale;
        /* pl_mpeg's val * sf >> 24, times 16 for the fixed-point synthesis */
        sample[i] = (int32_t)(((int64_t)val * sf + (1 << 19)) >> 20);
    }
}

void media_mp2_init(struct media_mp2 *d)
{
    memset(d, 0, sizeof(*d));
}

/* Output = -(sum of window2 * V) / 2^20: the window is doubled and the
 * subband samples are 16 times pl_mpeg's (which are 1/32768 of full scale
 * per unit of 2^16), so the scale is 2^(1 + 4 + 16 - 1). pl_mpeg divides
 * by 65 * 2^24 instead, which offsets its dequantization. */
static int16_t out_sample(int64_t u)
{
    int32_t v = -(int32_t)((u + (1 << 19)) >> 20);
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

int media_mp2_decode(struct media_mp2 *d, const uint8_t *frame, uint32_t len, int16_t *out)
{
    struct media_mp2_info h;
    struct bits b;

    if (media_mp2_header(frame, len, &h) || len < h.frame_len)
        return -1;
    b.p = frame;
    b.pos = 32 + (h.crc ? 16 : 0);
    b.end = h.frame_len * 8u;

    int tab3, sblimit;
    if (h.lsf) {
        tab3 = QUANT_TAB_LSF;
    } else {
        int tab2 = quant_lut_step_1[h.mode == 3 ? 0 : 1][h.bitrate_index];
        tab3 = quant_lut_step_2[tab2][h.rate_index];
    }
    sblimit = tab3 & 63;
    tab3 >>= 6;

    int channels = h.channels;
    int bound = h.mode == 1 ? (h.mode_ext + 1) << 2 : h.mode == 3 ? 0 : 32;
    if (bound > sblimit)
        bound = sblimit;

    for (int sb = 0; sb < bound; sb++) {
        d->allocation[0][sb] = read_allocation(&b, sb, tab3);
        d->allocation[1][sb] = read_allocation(&b, sb, tab3);
    }
    for (int sb = bound; sb < sblimit; sb++)
        d->allocation[0][sb] = d->allocation[1][sb] = read_allocation(&b, sb, tab3);

    for (int sb = 0; sb < sblimit; sb++)
        for (int ch = 0; ch < channels; ch++)
            if (d->allocation[ch][sb])
                d->scale_factor_info[ch][sb] = (uint8_t)rd(&b, 2);

    for (int sb = 0; sb < sblimit; sb++) {
        for (int ch = 0; ch < channels; ch++) {
            if (!d->allocation[ch][sb])
                continue;
            int32_t *sf = d->scale_factor[ch][sb];
            switch (d->scale_factor_info[ch][sb]) {
            case 0:
                sf[0] = (int32_t)rd(&b, 6);
                sf[1] = (int32_t)rd(&b, 6);
                sf[2] = (int32_t)rd(&b, 6);
                break;
            case 1:
                sf[0] = sf[1] = (int32_t)rd(&b, 6);
                sf[2] = (int32_t)rd(&b, 6);
                break;
            case 2:
                sf[0] = sf[1] = sf[2] = (int32_t)rd(&b, 6);
                break;
            case 3:
                sf[0] = (int32_t)rd(&b, 6);
                sf[1] = sf[2] = (int32_t)rd(&b, 6);
                break;
            }
        }
    }

    int out_pos = 0;
    for (int part = 0; part < 3; part++) {
        for (int granule = 0; granule < 4; granule++) {
            for (int sb = 0; sb < bound; sb++) {
                read_samples(d, &b, 0, sb, part);
                read_samples(d, &b, 1, sb, part);
            }
            for (int sb = bound; sb < sblimit; sb++) {
                read_samples(d, &b, 0, sb, part);
                memcpy(d->sample[1][sb], d->sample[0][sb], sizeof(d->sample[0][sb]));
            }
            for (int sb = sblimit; sb < 32; sb++)
                memset(d->sample[0][sb], 0, sizeof(d->sample[0][sb])),
                memset(d->sample[1][sb], 0, sizeof(d->sample[1][sb]));

            for (int p = 0; p < 3; p++) {
                d->v_pos = (d->v_pos - 64) & 1023;
                int d0 = 512 - (d->v_pos >> 1);
                int v0 = (d->v_pos % 128) >> 1, v1 = 96 - v0;
                for (int ch = 0; ch < channels; ch++) {
                    const int32_t *V = d->V[ch];
                    idct32(d->sample[ch], p, d->V[ch], d->v_pos);
                    /* pl_mpeg's two passes of eight rows, summed per output */
                    for (int i = 0; i < 32; i++) {
                        int64_t u = 0;
                        for (int r = 0; r < 8; r++) {
                            u += (int64_t)window2[(d0 + 64 * r + i) & 511] * V[v0 + 128 * r + i];
                            u += (int64_t)window2[(d0 + 32 + 64 * r + i) & 511] * V[v1 + 128 * r + i];
                        }
                        out[(out_pos + i) * channels + ch] = out_sample(u);
                    }
                }
                out_pos += 32;
            }
        }
    }
    return MEDIA_MP2_FRAME_SAMPLES;
}
