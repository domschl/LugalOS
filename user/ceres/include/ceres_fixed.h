/* user/ceres/include/ceres_fixed.h — 16.16 Signed Fixed-Point Math & LUT */
#ifndef CERES_FIXED_H
#define CERES_FIXED_H

#include <stdint.h>
#include <stdbool.h>

/*
 * 16.16 Signed Fixed-Point Arithmetic (fixed32_t)
 * Strictly zero floating point — fully compliant with RISC-V mstatus.FS = 0.
 */
typedef int32_t fixed32_t;

#define FIXED_SHIFT         16
#define FIXED_ONE           ((fixed32_t)(1 << FIXED_SHIFT))
#define FIXED_HALF          ((fixed32_t)(1 << (FIXED_SHIFT - 1)))
#define FIXED_ZERO          ((fixed32_t)0)

#define INT_TO_FIXED(x)     ((fixed32_t)((uint32_t)(x) << FIXED_SHIFT))
#define FIXED_TO_INT(x)     ((int32_t)((x) >> FIXED_SHIFT))
#define FIXED_ROUND(x)      ((int32_t)(((x) + FIXED_HALF) >> FIXED_SHIFT))

static inline fixed32_t fixed_from_int(int32_t x) {
    return INT_TO_FIXED(x);
}

static inline int32_t fixed_to_int(fixed32_t x) {
    return FIXED_TO_INT(x);
}

static inline fixed32_t fixed_mul(fixed32_t a, fixed32_t b) {
    return (fixed32_t)(((int64_t)a * (int64_t)b) >> FIXED_SHIFT);
}

static inline fixed32_t fixed_div(fixed32_t a, fixed32_t b) {
    if (b == 0) return (a >= 0) ? INT32_MAX : INT32_MIN;
    return (fixed32_t)(((int64_t)a * FIXED_ONE) / b);
}

static inline fixed32_t fixed_abs(fixed32_t x) {
    return (x < 0) ? -x : x;
}

static inline fixed32_t fixed_clamp(fixed32_t x, fixed32_t min_v, fixed32_t max_v) {
    if (x < min_v) return min_v;
    if (x > max_v) return max_v;
    return x;
}

static inline fixed32_t fixed_lerp(fixed32_t a, fixed32_t b, fixed32_t t) {
    return a + fixed_mul(b - a, t);
}

/* Trigonometric LUT: 256 steps for 360 degrees (0..255 = 0..2*pi) */
fixed32_t fixed_sin(uint8_t angle_256);
fixed32_t fixed_cos(uint8_t angle_256);

#endif /* CERES_FIXED_H */
