/*
 * drivers/radio/uprintf.c on the host (45.3b, plan/phase45_esp32c6.md).
 *
 * Differential: the same format and arguments through the C library's snprintf
 * and through ku_snprintf, over every flag, width, precision and length
 * modifier the formatter claims to support, with values chosen from the edges
 * (0, 1, -1, INT_MIN, UINT_MAX, 64-bit extremes) and from the PRNG. The
 * C library is the oracle; a difference is a bug here. Truncation, the return
 * value and NUL termination are checked for every buffer size from 0 to the
 * full length.
 *
 * Usage: uprintf_host [iterations [seed]].
 */

#include "radio/uprintf.h"
#include "shim.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); abort(); } } while (0)

static uint64_t g_rs;
static uint32_t rnd(uint32_t n) { return host_rand(&g_rs) % n; }

static const long long EDGE[] = { 0, 1, -1, 7, 10, 255, 256, 65535, 65536, 2147483647LL, -2147483648LL,
    4294967295LL, 4294967296LL, 9223372036854775807LL, (-9223372036854775807LL - 1), 123456789012345LL };

static long long pick(void) {
    if (rnd(3)) return EDGE[rnd(sizeof EDGE / sizeof EDGE[0])];
    return (long long)(((uint64_t)host_rand(&g_rs) << 32) | host_rand(&g_rs)) >> rnd(60);
}

static void compare(const char *fmt, ...) {
    char want[512], got[512];
    va_list a, b;
    va_start(a, fmt); va_copy(b, a);
    int rw = vsnprintf(want, sizeof want, fmt, a);
    int rg = ku_vsnprintf(got, sizeof got, fmt, b);
    va_end(a); va_end(b);
    CHECK(rw == rg && strcmp(want, got) == 0, "format \"%s\": libc \"%s\" (%d), ours \"%s\" (%d)", fmt, want, rw, got, rg);
}

static void test_fixed(void) {
    compare("plain text");
    compare("%d|%i|%u|%x|%X|%o", -42, 42, 42u, 0xbeefu, 0xbeefu, 8u);
    compare("%5d|%-5d|%05d|%+d|% d", 42, 42, 42, 42, 42);
    compare("%.3d|%8.3d|%-8.3d|%08.3d", 7, 7, 7, 7);
    compare("%#x|%#X|%#o|%#x", 255u, 255u, 8u, 0u);
    compare("%c%c%c", 'a', 'b', 'c');
    compare("%s|%10s|%-10s|%.3s|%10.3s", "hello", "hello", "hello", "hello", "hello");
    compare("%.0d|%.0u|%.0x", 0, 0u, 0u);
    compare("%ld|%lu|%lx", -1234567L, 1234567UL, 0xdeadbeefUL);
    compare("%lld|%llu|%llx", -123456789012345LL, 123456789012345ULL, 0xdeadbeefcafef00dULL);
    compare("%hhd|%hd|%hhu|%hu", 300, 70000, 300, 70000);
    compare("%zu|%zd", (size_t)123, (ssize_t)-5);
    compare("%*d|%-*d|%.*s", 6, 42, 6, 42, 2, "hello");
    compare("100%%");
    compare("%s", (char *)NULL == NULL ? "(null)" : "");
    compare("%02x:%02x:%02x:%02x:%02x:%02x", 0xac, 0xeb, 0xe6, 0x1e, 0x3a, 0xac);
    compare("rssi %d dBm, ch %u, bw %uMHz, %s", -61, 6u, 20u, "WPA2");
}

static void test_random(int iters) {
    static const char *flags[] = { "", "-", "0", "+", " ", "#", "-+", "0+", "# ", "-#" };
    static const char *lens[]  = { "", "hh", "h", "l", "ll", "z" };
    static const char convs[]  = { 'd', 'i', 'u', 'x', 'X', 'o' };
    char fmt[64];
    for (int it = 0; it < iters; it++) {
        char conv = convs[rnd(sizeof convs)];
        const char *len = lens[rnd(sizeof lens / sizeof lens[0])];
        char w[8] = "", pr[8] = "";
        if (rnd(2)) snprintf(w, sizeof w, "%u", rnd(20));
        if (rnd(2)) snprintf(pr, sizeof pr, ".%u", rnd(16));
        snprintf(fmt, sizeof fmt, "[%%%s%s%s%s%c]", flags[rnd(sizeof flags / sizeof flags[0])], w, pr, len, conv);
        long long v = pick();
        if (!strcmp(len, "ll"))      compare(fmt, v);
        else if (!strcmp(len, "l"))  compare(fmt, (long)v);
        else if (!strcmp(len, "z"))  compare(fmt, (size_t)v);
        else                         compare(fmt, (int)v);
    }
    /* strings with width and precision */
    static const char *strs[] = { "", "a", "hello", "a longer string than any width here", "tab\there" };
    for (int it = 0; it < iters / 4; it++) {
        char w[8] = "", pr[8] = "";
        if (rnd(2)) snprintf(w, sizeof w, "%u", rnd(40));
        if (rnd(2)) snprintf(pr, sizeof pr, ".%u", rnd(12));
        snprintf(fmt, sizeof fmt, "<%%%s%s%ss>", rnd(2) ? "-" : "", w, pr);
        compare(fmt, strs[rnd(sizeof strs / sizeof strs[0])]);
    }
}

/* Every buffer size from 0 up, for one longish line: the return value is the
 * length snprintf would have written, the output is a NUL-terminated prefix,
 * and nothing is written past the buffer (ASan watches). */
static void test_truncation(void) {
    const char *f = "wifi:mode : %s (%02x:%02x:%02x) ch=%d %5.2s|%-8u|";
    char want[200];
    int full = snprintf(want, sizeof want, f, "sta", 0xac, 0xeb, 0xe6, -7, "abcdef", 99u);
    for (int cap = 0; cap <= full + 2; cap++) {
        char *b = malloc((size_t)cap + 1 > 1 ? (size_t)cap : 1);          /* exactly cap bytes, so ASan sees an overrun */
        int r = ku_snprintf(b, (uint32_t)cap, f, "sta", 0xac, 0xeb, 0xe6, -7, "abcdef", 99u);
        CHECK(r == full, "cap %d: returned %d, want %d", cap, r, full);
        if (cap > 0) {
            size_t want_len = (size_t)cap - 1 < (size_t)full ? (size_t)cap - 1 : (size_t)full;
            CHECK(strlen(b) == want_len && memcmp(b, want, want_len) == 0, "cap %d: prefix wrong", cap);
        }
        free(b);
    }
}

static void test_odd(void) {
    char b[64];
    CHECK(ku_snprintf(b, sizeof b, "%q", 1) == 2 && !strcmp(b, "%q"), "an unknown conversion prints as itself");
    CHECK(ku_snprintf(b, sizeof b, "%f|%d", 1.5, 7) == 3 && !strcmp(b, "?|7"), "a float consumes its argument: %s", b);
    CHECK(ku_snprintf(b, sizeof b, "%s", (char *)NULL) == 6 && !strcmp(b, "(null)"), "NULL string");
    CHECK(ku_snprintf(b, sizeof b, "tail %") == 5, "a lone % at the end does not run off the string");
    CHECK(ku_snprintf(b, sizeof b, "%p", (void *)0x1234) == 6 && !strcmp(b, "0x1234"), "%%p: %s", b);
}

int main(int argc, char **argv) {
    int iterations = argc > 1 ? atoi(argv[1]) : 2000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x40c0ffee;
    g_rs = seed | 1;
    printf("uprintf_host: seed %#llx, %d random formats\n", (unsigned long long)seed, iterations);
    test_fixed();
    test_odd();
    test_truncation();
    test_random(iterations * 20);
    printf("uprintf_host: agrees with the C library (%d random + fixed + truncation)\n", iterations * 20);
    return 0;
}
