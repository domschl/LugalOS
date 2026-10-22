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

#endif /* MOON_FIXED_H */
