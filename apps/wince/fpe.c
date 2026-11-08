/* COREDLL's floating-point emulation helpers, C math and setjmp/longjmp.
 *
 * The BE-300's VR4131 has no FPU, so Microsoft's MIPS compiler calls these
 * helpers for every double (__dp*) and float (__fp*) operation. Their
 * register use is O32's soft-float convention, which GCC's -msoft-float
 * shares: doubles in a0/a1 and a2/a3, results in v0(/v1). The arithmetic is
 * compiler-rt's (third_party/compiler-rt) and the math functions fdlibm's,
 * as JerryScript packages it (third_party/jerryscript/jerryscript-math.c).
 * Comparisons return 1 or 0: Internet.exe branches on beqz/bnez. */
#include "runtime.h"

double acos(double);
double asin(double);
double atan(double);
double atan2(double, double);
double ceil(double);
double cos(double);
double exp(double);
double fabs(double);
double floor(double);
double fmod(double, double);
double log(double);
double log10(double);
double pow(double, double);
double sin(double);
double sqrt(double);
double tan(double);

static double dpadd(double a, double b) { return a + b; }
static double dpsub(double a, double b) { return a - b; }
static double dpmul(double a, double b) { return a * b; }
static double dpdiv(double a, double b) { return a / b; }
static float fpadd(float a, float b) { return a + b; }
static float fpsub(float a, float b) { return a - b; }
static float fpmul(float a, float b) { return a * b; }
static float fpdiv(float a, float b) { return a / b; }
static int eqd(double a, double b) { return a == b; }
static int ned(double a, double b) { return a != b; }
static int ltd(double a, double b) { return a < b; }
static int led(double a, double b) { return a <= b; }
static int gtd(double a, double b) { return a > b; }
static int ged(double a, double b) { return a >= b; }
static int eqs(float a, float b) { return a == b; }
static int nes(float a, float b) { return a != b; }
static int lts(float a, float b) { return a < b; }
static int les(float a, float b) { return a <= b; }
static int gts(float a, float b) { return a > b; }
static int ges(float a, float b) { return a >= b; }
static double litodp(int32_t v) { return v; }
static double ultodp(uint32_t v) { return v; }
static float litofp(int32_t v) { return (float)v; }
static float ultofp(uint32_t v) { return (float)v; }
static double fptodp(float v) { return v; }
static float dptofp(double v) { return (float)v; }
/* Out-of-range conversions are undefined in C; return CE's saturated values
 * instead of leaving them to the soft-float library. */
static int32_t dptoli(double v)
{
    return v != v ? 0 : v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : (int32_t)v;
}
static uint32_t dptoul(double v)
{
    return v != v || v <= 0 ? 0 : v >= 4294967295.0 ? UINT32_MAX : (uint32_t)v;
}
static int32_t fptoli(float v) { return dptoli(v); }
static uint32_t fptoul(float v) { return dptoul(v); }
static uint64_t d_to_ull(double v)
{
    return v != v || v <= 0 ? 0 : v >= 18446744073709551615.0 ? UINT64_MAX : (uint64_t)v;
}
static int64_t d_to_ll(double v)
{
    return v != v ? 0 : v >= 9223372036854775807.0 ? INT64_MAX : v <= -9223372036854775808.0 ? INT64_MIN : (int64_t)v;
}
static double ll_to_d(int64_t v) { return (double)v; }
static double ull_to_d(uint64_t v) { return (double)v; }
static double neg(double v) { return -v; }
static float negs(float v) { return -v; }

/* _gcvt(value, digits, buffer): %g-style text with `digits` significant
 * digits, as Microsoft's CRT writes it (exponent form outside 1e-4..1e+digits). */
static char *gcvt(double value, int digits, char *buffer)
{
    char *p = buffer;
    if (digits < 1)
        digits = 1;
    if (digits > 17)
        digits = 17;
    if (value != value) {
        strcpy(buffer, "nan");
        return buffer;
    }
    if (value < 0) {
        *p++ = '-';
        value = -value;
    }
    if (value > 1.7976931348623157e308) {
        strcpy(p, "inf");
        return buffer;
    }
    int exponent = 0;
    if (value != 0) {
        while (value >= 10) {
            value /= 10;
            exponent++;
        }
        while (value < 1) {
            value *= 10;
            exponent--;
        }
    }
    char digit[18];
    for (int i = 0; i < digits; i++) {
        int d = (int)value;
        digit[i] = (char)('0' + d);
        value = (value - d) * 10;
    }
    if (value >= 5) { /* round the last digit, carrying left */
        int i = digits - 1;
        while (i >= 0 && digit[i] == '9')
            digit[i--] = '0';
        if (i >= 0)
            digit[i]++;
        else {
            memmove(digit + 1, digit, (size_t)digits - 1);
            digit[0] = '1';
            exponent++;
        }
    }
    int used = digits;
    while (used > 1 && digit[used - 1] == '0')
        used--;
    if (exponent < -4 || exponent >= digits) {
        *p++ = digit[0];
        if (used > 1) {
            *p++ = '.';
            memcpy(p, digit + 1, (size_t)used - 1);
            p += used - 1;
        }
        p += snprintf(p, 6, "e%c%03d", exponent < 0 ? '-' : '+', exponent < 0 ? -exponent : exponent);
    } else if (exponent < 0) {
        *p++ = '0';
        *p++ = '.';
        for (int i = -1; i > exponent; i--)
            *p++ = '0';
        memcpy(p, digit, (size_t)used);
        p += used;
    } else {
        for (int i = 0; i <= exponent; i++)
            *p++ = i < used ? digit[i] : '0';
        if (used > exponent + 1) {
            *p++ = '.';
            memcpy(p, digit + exponent + 1, (size_t)(used - exponent - 1));
            p += used - exponent - 1;
        }
    }
    *p = 0;
    return buffer;
}

/* wcstod(text, end): decimal digits, fraction and exponent, as the CE CRT
 * accepts them after leading white space. */
static double wcstod_(const WCHAR *text, WCHAR **end)
{
    const WCHAR *p = text;
    while (*p == ' ' || (*p >= 9 && *p <= 13))
        p++;
    bool negative = *p == '-';
    if (*p == '-' || *p == '+')
        p++;
    double value = 0;
    int scale = 0, digits = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p++ - '0');
        digits++;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p++ - '0');
            scale--;
            digits++;
        }
    }
    if (!digits) {
        if (end)
            *end = (WCHAR *)text;
        return 0;
    }
    if (*p == 'e' || *p == 'E') {
        const WCHAR *mark = p++;
        bool minus = *p == '-';
        if (*p == '-' || *p == '+')
            p++;
        if (*p >= '0' && *p <= '9') {
            int e = 0;
            while (*p >= '0' && *p <= '9')
                e = e < 10000 ? e * 10 + (*p++ - '0') : (p++, e);
            scale += minus ? -e : e;
        } else {
            p = mark;
        }
    }
    if (scale)
        value = scale < 0 ? value / pow(10, -scale) : value * pow(10, scale);
    if (end)
        *end = (WCHAR *)p;
    return negative ? -value : value;
}

/* setjmp/longjmp keep the callee-saved registers, sp, ra and gp in the
 * caller's jmp_buf (at least 16 words on CE MIPS). Bridges reach these with
 * a plain jump, so ra and sp are still the CE caller's. */
__asm__(".text\n"
        ".set push\n.set noreorder\n.set nomacro\n"
        ".globl ce_setjmp_asm\n.type ce_setjmp_asm,@function\n"
        "ce_setjmp_asm:\n"
        " sw $16,0($4)\n sw $17,4($4)\n sw $18,8($4)\n sw $19,12($4)\n"
        " sw $20,16($4)\n sw $21,20($4)\n sw $22,24($4)\n sw $23,28($4)\n"
        " sw $30,32($4)\n sw $29,36($4)\n sw $31,40($4)\n sw $28,44($4)\n"
        " jr $31\n move $2,$0\n"
        ".size ce_setjmp_asm,.-ce_setjmp_asm\n"
        ".globl ce_longjmp_asm\n.type ce_longjmp_asm,@function\n"
        "ce_longjmp_asm:\n"
        " lw $16,0($4)\n lw $17,4($4)\n lw $18,8($4)\n lw $19,12($4)\n"
        " lw $20,16($4)\n lw $21,20($4)\n lw $22,24($4)\n lw $23,28($4)\n"
        " lw $30,32($4)\n lw $29,36($4)\n lw $31,40($4)\n lw $28,44($4)\n"
        " bnez $5,1f\n move $2,$5\n addiu $2,$0,1\n"
        "1: jr $31\n nop\n"
        ".size ce_longjmp_asm,.-ce_longjmp_asm\n"
        ".set pop\n");
void ce_setjmp_asm(void);
void ce_longjmp_asm(void);

static const struct ce_export exports[] = {
    {"__dpadd", (void *)dpadd},   {"__dpsub", (void *)dpsub},     {"__dpmul", (void *)dpmul},
    {"__dpdiv", (void *)dpdiv},   {"__fpadd", (void *)fpadd},     {"__fpsub", (void *)fpsub},
    {"__fpmul", (void *)fpmul},   {"__fpdiv", (void *)fpdiv},     {"__eqd", (void *)eqd},
    {"__ned", (void *)ned},       {"__ltd", (void *)ltd},         {"__led", (void *)led},
    {"__gtd", (void *)gtd},       {"__ged", (void *)ged},         {"__eqs", (void *)eqs},
    {"__nes", (void *)nes},       {"__lts", (void *)lts},         {"__les", (void *)les},
    {"__gts", (void *)gts},       {"__ges", (void *)ges},         {"__litodp", (void *)litodp},
    {"__ultodp", (void *)ultodp}, {"__litofp", (void *)litofp},   {"__ultofp", (void *)ultofp},
    {"__fptodp", (void *)fptodp}, {"__dptofp", (void *)dptofp},   {"__dptoli", (void *)dptoli},
    {"__dptoul", (void *)dptoul}, {"__fptoli", (void *)fptoli},   {"__fptoul", (void *)fptoul},
    {"__d_to_ull", (void *)d_to_ull}, {"__d_to_ll", (void *)d_to_ll}, {"__ll_to_d", (void *)ll_to_d},
    {"__ull_to_d", (void *)ull_to_d}, {"__dpneg", (void *)neg},   {"__fpneg", (void *)negs},
    {"acos", (void *)acos},       {"asin", (void *)asin},         {"atan", (void *)atan},
    {"atan2", (void *)atan2},     {"ceil", (void *)ceil},         {"cos", (void *)cos},
    {"exp", (void *)exp},         {"fabs", (void *)fabs},         {"floor", (void *)floor},
    {"fmod", (void *)fmod},       {"log", (void *)log},           {"log10", (void *)log10},
    {"pow", (void *)pow},         {"sin", (void *)sin},           {"sqrt", (void *)sqrt},
    {"tan", (void *)tan},         {"_gcvt", (void *)gcvt},        {"wcstod", (void *)wcstod_},
    {"_setjmp", (void *)ce_setjmp_asm}, {"setjmp", (void *)ce_setjmp_asm}, {"longjmp", (void *)ce_longjmp_asm},
};
void *ce_resolve_fpe(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}

/* Data exports bind the import slot to the variable itself, not a bridge. */
static const double huge = __builtin_inf();
void *ce_resolve_data(const char *dll, const char *name)
{
    if (!strcasecmp(dll, "coredll.dll") && !strcmp(name, "_HUGE"))
        return (void *)&huge;
    return NULL;
}
