/* lib/libc's vsnprintf against the host's, natively. printf.c is built with
 * its symbols renamed and glaze/sys.h replaced (host/Makefile), so the
 * host's snprintf is still there to compare with. Every case also checks
 * that the arguments after a conversion stay in step: a length modifier
 * read with the wrong size used to shift them (a 64-bit cookie time in
 * Browser's jar became a string pointer). */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

int gz_snprintf(char *buf, size_t n, const char *fmt, ...);

int gz_log(const char *s, uint32_t len)
{
    return (int)fwrite(s, 1, len, stdout);
}

static int failures, cases;

static void check(const char *fmt, const char *ours, int our_len, const char *host, int host_len)
{
    cases++;
    if (our_len != host_len || strcmp(ours, host)) {
        printf("FAIL %s: libc \"%s\" (%d), host \"%s\" (%d)\n", fmt, ours, our_len, host, host_len);
        failures++;
    }
}

/* Both formatters on the same arguments, with a string after them. */
#define SAME(fmt, ...) do { \
    char a_[160], b_[160]; \
    int la_ = gz_snprintf(a_, sizeof a_, fmt "|%s", __VA_ARGS__, "end"); \
    int lb_ = snprintf(b_, sizeof b_, fmt "|%s", __VA_ARGS__, "end"); \
    check(fmt, a_, la_, b_, lb_); \
} while (0)

int main(void)
{
    /* What libc already did. */
    SAME("%d %i %u %x %X", 0, -42, 4000000000u, 0xbeefu, 0xbeefu);
    SAME("[%5d] [%05d] [%-5d] [%-5d]", 42, -42, 42, -7);
    SAME("[%s] [%8s] [%-8s] [%.3s] [%.*s]", "abc", "abc", "abc", "abcdef", 2, "xyz");
    SAME("[%*d] [%-*d] [%*d]", 6, 12, 6, 12, -6, 12);
    SAME("%c%c %% %s", 'o', 'k', "x");
    SAME("%d %d", INT_MIN, INT_MAX);

    /* 64-bit integers: the case that crashed. */
    long long expires = 1798761600LL;
    SAME("%lld\t%d\t%s\t%s", expires, 5, "google.com", "/");
    SAME("%lld %lld %llu", LLONG_MIN, LLONG_MAX, ULLONG_MAX);
    SAME("%llx %llX %llo", 0x123456789abcdefULL, 0xfedcba9876543210ULL, 01777777777777777777777ULL);
    SAME("[%20lld] [%-20lld] [%020lld]", -1234567890123LL, 1234567890123LL, -99LL);
    SAME("%d %lld %d", 1, 1LL << 40, 2);
    SAME("%jd %ju", (intmax_t)-5000000000LL, (uintmax_t)5000000000ULL);

    /* Host-sized types: long, size_t, ptrdiff_t. */
    SAME("%ld %lu %lx", -123456L, 123456UL, 0xabcdefUL);
    SAME("%zu %zd %zx", (size_t)4096, (ssize_t)-1, (size_t)0xffff);
    SAME("%td", (ptrdiff_t)-77);

    /* Narrow types are converted back after promotion. */
    SAME("%hd %hu %hhd %hhu %hx", (short)-2, (unsigned short)65535, (signed char)-3,
         (unsigned char)250, (unsigned short)0x12345);
    SAME("%hd %hhu", 70000, 300);

    SAME("%o %5o %-5o|", 8u, 8u, 8u);

    /* Truncation still counts the full length and terminates. */
    {
        char a[8], b[8];
        int la = gz_snprintf(a, sizeof a, "%lld-%s", 123456789012LL, "tail");
        int lb = snprintf(b, sizeof b, "%lld-%s", 123456789012LL, "tail");
        check("truncated %lld", a, la, b, lb);
        la = gz_snprintf(NULL, 0, "%llu", ULLONG_MAX);
        check("length only", "", la, "", (int)strlen("18446744073709551615"));
    }

    /* A trailing '%' stops at the end of the format instead of reading on. */
    {
        char a[16];
        int la = gz_snprintf(a, sizeof a, "50%");
        check("trailing %", a, la, "50", 2);
    }

    printf("libc printf: %d cases, %d failures\n", cases, failures);
    return failures != 0;
}
