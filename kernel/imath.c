#include "kernel/imath.h"
#include "kernel/console.h"

uint32_t imath_isqrt64(uint64_t v) {
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* atan(x) for x = z / 10000 in [0, 1], in tenths of a degree:
 * atan(x) ~ 45 x + x (1 - x) (14.02 + 3.80 x) degrees, error under 0.1
 * degree (the rad form is pi/4 x - x (x - 1)(0.2447 + 0.0663 x)). */
static int32_t atan01_deg10(int64_t z) {
    int64_t lin = 450 * z;                                    /* x 1e4 */
    int64_t corr = z * (10000 - z) * (14020000 + 380 * z);    /* x 1e13 */
    return (int32_t)((lin * 1000000000LL + corr + 5000000000000LL) / 10000000000000LL);
}

int32_t imath_atan2_deg10(int32_t y, int32_t x) {
    if (x == 0 && y == 0) return 0;
    int64_t ax = x < 0 ? -(int64_t)x : x, ay = y < 0 ? -(int64_t)y : y;
    int32_t a = ax >= ay ? atan01_deg10(ay * 10000 / ax)
                         : 900 - atan01_deg10(ax * 10000 / ay);
    if (x < 0) a = 1800 - a;
    return y < 0 ? -a : a;
}

uint32_t imath_selftest(bool report) {
    uint32_t failed = 0;
    static const struct { uint64_t v; uint32_t r; } sq[] = {
        { 0, 0 }, { 1, 1 }, { 3, 1 }, { 4, 2 }, { 1000000, 1000 }, { 999999, 999 },
        { 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFu },
    };
    for (unsigned i = 0; i < sizeof sq / sizeof sq[0]; i++) {
        if (imath_isqrt64(sq[i].v) != sq[i].r) {
            failed++;
            if (report) cprintf("  isqrt case %u failed\n", i);
        }
    }
    /* want: round(10 * degrees(atan2(y, x))) */
    static const struct { int32_t y, x, want; } at[] = {
        { 0, 1, 0 }, { 1, 1, 450 }, { 1, 0, 900 }, { 1, -1, 1350 }, { 0, -1, 1800 },
        { -1, 1, -450 }, { -1, 0, -900 }, { -1, -1, -1350 }, { 1, 2, 266 }, { 2, 1, 634 },
        { 1000, 1732, 300 }, { 1732, 1000, 600 }, { -1003, 12, -893 }, { 17, 1000, 10 },
    };
    for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++) {
        int32_t got = imath_atan2_deg10(at[i].y, at[i].x);
        int32_t d = got - at[i].want;
        if (d < -1 || d > 1) {
            failed++;
            if (report) cprintf("  atan2(%ld, %ld) = %ld, want %ld\n",
                               (long)at[i].y, (long)at[i].x, (long)got, (long)at[i].want);
        }
    }
    return failed;
}
