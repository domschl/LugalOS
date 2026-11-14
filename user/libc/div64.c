#include <stdint.h>

/* Position-independent (-mcmodel=medany) 64-bit integer helpers for RV32
 * U-mode programs. System libgcc.a is compiled with medlow absolute addresses,
 * which fault when loaded at an arbitrary base. */

uint64_t __ashldi3(uint64_t a, int b) {
    uint32_t s = (uint32_t)b & 63u;
    if (s == 0) return a;
    uint32_t lo = (uint32_t)a;
    uint32_t hi = (uint32_t)(a >> 32);
    if (s >= 32u) {
        hi = lo << (s - 32u);
        lo = 0;
    } else {
        hi = (hi << s) | (lo >> (32u - s));
        lo = lo << s;
    }
    return ((uint64_t)hi << 32) | lo;
}

uint64_t __lshrdi3(uint64_t a, int b) {
    uint32_t s = (uint32_t)b & 63u;
    if (s == 0) return a;
    uint32_t lo = (uint32_t)a;
    uint32_t hi = (uint32_t)(a >> 32);
    if (s >= 32u) {
        lo = hi >> (s - 32u);
        hi = 0;
    } else {
        lo = (lo >> s) | (hi << (32u - s));
        hi = hi >> s;
    }
    return ((uint64_t)hi << 32) | lo;
}

int64_t __ashrdi3(int64_t a, int b) {
    uint32_t s = (uint32_t)b & 63u;
    if (s == 0) return a;
    uint32_t lo = (uint32_t)(uint64_t)a;
    int32_t  hi = (int32_t)((uint64_t)a >> 32);
    if (s >= 32u) {
        lo = (uint32_t)(hi >> (s - 32u));
        hi = hi >> 31;
    } else {
        lo = (lo >> s) | ((uint32_t)hi << (32u - s));
        hi = hi >> s;
    }
    return (int64_t)(((uint64_t)(uint32_t)hi << 32) | lo);
}

static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem_out) {
    if (d == 0) {
        if (rem_out) *rem_out = 0;
        return 0;
    }
    uint64_t q = 0;
    uint64_t r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ULL);
        if (r >= d) {
            r -= d;
            q |= (1ULL << i);
        }
    }
    if (rem_out) *rem_out = r;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) {
    return udivmod64(n, d, (uint64_t *)0);
}

uint64_t __umoddi3(uint64_t n, uint64_t d) {
    uint64_t r = 0;
    (void)udivmod64(n, d, &r);
    return r;
}

int64_t __divdi3(int64_t n, int64_t d) {
    int neg = (n < 0) ^ (d < 0);
    uint64_t un = (n < 0) ? (0ULL - (uint64_t)n) : (uint64_t)n;
    uint64_t ud = (d < 0) ? (0ULL - (uint64_t)d) : (uint64_t)d;
    uint64_t q = udivmod64(un, ud, (uint64_t *)0);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t n, int64_t d) {
    int neg = (n < 0);
    uint64_t un = (n < 0) ? (0ULL - (uint64_t)n) : (uint64_t)n;
    uint64_t ud = (d < 0) ? (0ULL - (uint64_t)d) : (uint64_t)d;
    uint64_t r = 0;
    (void)udivmod64(un, ud, &r);
    return neg ? -(int64_t)r : (int64_t)r;
}
