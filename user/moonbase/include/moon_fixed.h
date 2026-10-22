#ifndef MOON_FIXED_H
#define MOON_FIXED_H

#include <stdint.h>

/*
 * 16.16 Signed Fixed-Point Arithmetic
 * Pure integer implementation — strictly zero hardware/software floating point.
 */
typedef int32_t fixed32_t;

#define FIXED_SHIFT         16
#define FIXED_ONE           ((fixed32_t)(1 << FIXED_SHIFT))
#define FIXED_HALF          ((fixed32_t)(1 << (FIXED_SHIFT - 1)))
#define FIXED_ZERO          ((fixed32_t)0)

#define INT_TO_FIXED(x)     ((fixed32_t)((int32_t)(x) << FIXED_SHIFT))
#define FIXED_TO_INT(x)     ((int32_t)((x) >> FIXED_SHIFT))
#define FIXED_ROUND(x)      ((int32_t)(((x) + FIXED_HALF) >> FIXED_SHIFT))

/* Multiplication: (a * b) >> 16 using 64-bit intermediate */
static inline fixed32_t fixed_mul(fixed32_t a, fixed32_t b) {
    return (fixed32_t)(((int64_t)a * (int64_t)b) >> FIXED_SHIFT);
}

/* Division: (a << 16) / b using 64-bit intermediate */
static inline fixed32_t fixed_div(fixed32_t a, fixed32_t b) {
    if (b == 0) return (a >= 0) ? INT32_MAX : INT32_MIN;
    return (fixed32_t)((((int64_t)a) << FIXED_SHIFT) / b);
}

static inline fixed32_t fixed_abs(fixed32_t x) {
    return (x < 0) ? -x : x;
}

static inline fixed32_t fixed_clamp(fixed32_t x, fixed32_t min_v, fixed32_t max_v) {
    if (x < min_v) return min_v;
    if (x > max_v) return max_v;
    return x;
}

/* Linear interpolation: a + (b - a) * t (where t is 0..FIXED_ONE) */
static inline fixed32_t fixed_lerp(fixed32_t a, fixed32_t b, fixed32_t t) {
    return a + fixed_mul(b - a, t);
}

/*
 * Fast integer square root for 16.16 fixed-point numbers.
 * sqrt(x * 2^16) = sqrt(x * 2^32) >> 8 = isqrt64((uint64_t)x << 16)
 */
static inline fixed32_t fixed_sqrt(fixed32_t x) {
    if (x <= 0) return 0;
    uint64_t val = (uint64_t)x << FIXED_SHIFT;
    uint64_t res = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > val) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (val >= res + bit) {
            val -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }

    return (fixed32_t)res;
}

/*
 * Sine table for 0 to 90 degrees (91 entries, scaled by 65536 = FIXED_ONE)
 */
extern const uint32_t g_fixed_sin_table_90[91];

/*
 * Calculate fixed-point sine of angle in degrees (0..359, handles any integer)
 */
static inline fixed32_t fixed_sin_deg(int32_t deg) {
    deg = deg % 360;
    if (deg < 0) deg += 360;

    if (deg <= 90) {
        return (fixed32_t)g_fixed_sin_table_90[deg];
    } else if (deg <= 180) {
        return (fixed32_t)g_fixed_sin_table_90[180 - deg];
    } else if (deg <= 270) {
        return -(fixed32_t)g_fixed_sin_table_90[deg - 180];
    } else {
        return -(fixed32_t)g_fixed_sin_table_90[360 - deg];
    }
}

/*
 * Calculate fixed-point cosine of angle in degrees
 */
static inline fixed32_t fixed_cos_deg(int32_t deg) {
    return fixed_sin_deg(deg + 90);
}

#endif /* MOON_FIXED_H */
