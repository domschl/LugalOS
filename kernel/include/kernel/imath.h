#ifndef KERNEL_IMATH_H
#define KERNEL_IMATH_H

#include <stdbool.h>
#include <stdint.h>

/* Integer square root: floor(sqrt(v)). */
uint32_t imath_isqrt64(uint64_t v);

/* atan2(y, x) in tenths of a degree, -1800..1800, without floating point.
 * Within 0.2 degree of the true value (a polynomial for atan on [0, 1],
 * folded into the other octants); 0 for (0, 0). */
int32_t imath_atan2_deg10(int32_t y, int32_t x);

/* Golden vectors; failures count. */
uint32_t imath_selftest(bool report);

#endif /* KERNEL_IMATH_H */
