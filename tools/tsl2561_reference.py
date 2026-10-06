#!/usr/bin/env python3
"""tools/tsl2561_reference.py -- Independent reference math for AMS/TAOS TSL2561.

Phase 46, plan/phase46_sensor_framework.md §4.6.
Computes ambient illuminance (Lux) and centi-Lux (0.01 Lux) using the exact
piecewise linear integer approximation from the TAOS TSL2561 datasheet (TAOS059N).
"""

from typing import NamedTuple

# Scaling factors from TAOS datasheet
LUX_SCALE = 14       # Scale by 2^14
RATIO_SCALE = 9      # Scale ratio by 2^9
CH_SCALE = 10        # Scale channel values by 2^10

CHSCALE_TINT0 = 0x7517  # 13.7 ms: 322/11 * 2^CH_SCALE
CHSCALE_TINT1 = 0x0fe7  # 101 ms: 322/81 * 2^CH_SCALE

# Coefficients for T, FN, CL package
K1T = 0x0040  # 0.125 * 2^RATIO_SCALE
B1T = 0x01f2  # 0.0304 * 2^LUX_SCALE
M1T = 0x01be  # 0.0272 * 2^LUX_SCALE

K2T = 0x0080  # 0.250 * 2^RATIO_SCALE
B2T = 0x0214  # 0.0325 * 2^LUX_SCALE
M2T = 0x02d1  # 0.0440 * 2^LUX_SCALE

K3T = 0x00c0  # 0.375 * 2^RATIO_SCALE
B3T = 0x023f  # 0.0351 * 2^LUX_SCALE
M3T = 0x037b  # 0.0544 * 2^LUX_SCALE

K4T = 0x0100  # 0.500 * 2^RATIO_SCALE
B4T = 0x0270  # 0.0381 * 2^LUX_SCALE
M4T = 0x03fe  # 0.0624 * 2^LUX_SCALE

K5T = 0x0138  # 0.610 * 2^RATIO_SCALE
B5T = 0x016f  # 0.0224 * 2^LUX_SCALE
M5T = 0x01fc  # 0.0310 * 2^LUX_SCALE

K6T = 0x019a  # 0.800 * 2^RATIO_SCALE
B6T = 0x00d2  # 0.0128 * 2^LUX_SCALE
M6T = 0x00fb  # 0.0153 * 2^LUX_SCALE

K7T = 0x029a  # 1.300 * 2^RATIO_SCALE
B7T = 0x0018  # 0.00146 * 2^LUX_SCALE
M7T = 0x0012  # 0.00112 * 2^LUX_SCALE

# Integration time modes
INTEG_13MS = 0   # 13.7 ms
INTEG_101MS = 1  # 101 ms
INTEG_402MS = 2  # 402 ms

# Gain modes
GAIN_1X = 0
GAIN_16X = 1


def calculate_lux_c100(ch0: int, ch1: int, gain: int = GAIN_16X, integ: int = INTEG_402MS) -> int:
    """Calculate illuminance in centi-Lux (0.01 Lux) using the datasheet integer model."""
    if ch0 == 0:
        return 0

    # 1. Scale channel values depending on integration time and gain
    if integ == INTEG_13MS:
        ch_scale = CHSCALE_TINT0
    elif integ == INTEG_101MS:
        ch_scale = CHSCALE_TINT1
    else:
        ch_scale = 1 << CH_SCALE

    if gain == GAIN_1X:
        ch_scale <<= 4

    channel0 = (ch0 * ch_scale) >> CH_SCALE
    channel1 = (ch1 * ch_scale) >> CH_SCALE

    if channel0 == 0:
        return 0

    # 2. Ratio calculation (Channel1 / Channel0)
    ratio1 = (channel1 << (RATIO_SCALE + 1)) // channel0
    ratio = (ratio1 + 1) >> 1

    # 3. Piecewise linear coefficients
    if ratio <= K1T:
        b, m = B1T, M1T
    elif ratio <= K2T:
        b, m = B2T, M2T
    elif ratio <= K3T:
        b, m = B3T, M3T
    elif ratio <= K4T:
        b, m = B4T, M4T
    elif ratio <= K5T:
        b, m = B5T, M5T
    elif ratio <= K6T:
        b, m = B6T, M6T
    elif ratio <= K7T:
        b, m = B7T, M7T
    else:
        return 0

    term0 = channel0 * b
    term1 = channel1 * m
    if term0 <= term1:
        return 0

    diff = term0 - term1
    # Convert from 2^LUX_SCALE to centi-Lux (x100)
    lux_c100 = (diff * 100 + (1 << (LUX_SCALE - 1))) >> LUX_SCALE
    return lux_c100


class TestCase(NamedTuple):
    ch0: int
    ch1: int
    gain: int
    integ: int
    expected_c100: int


TEST_VECTORS = [
    # 402ms, 16x gain (nominal)
    TestCase(1000, 100, GAIN_16X, INTEG_402MS, calculate_lux_c100(1000, 100, GAIN_16X, INTEG_402MS)),
    TestCase(1000, 200, GAIN_16X, INTEG_402MS, calculate_lux_c100(1000, 200, GAIN_16X, INTEG_402MS)),
    TestCase(1000, 300, GAIN_16X, INTEG_402MS, calculate_lux_c100(1000, 300, GAIN_16X, INTEG_402MS)),
    TestCase(1000, 450, GAIN_16X, INTEG_402MS, calculate_lux_c100(1000, 450, GAIN_16X, INTEG_402MS)),
    TestCase(1000, 550, GAIN_16X, INTEG_402MS, calculate_lux_c100(1000, 550, GAIN_16X, INTEG_402MS)),
    TestCase(1000, 700, GAIN_16X, INTEG_402MS, calculate_lux_c100(1000, 700, GAIN_16X, INTEG_402MS)),
    TestCase(1000, 900, GAIN_16X, INTEG_402MS, calculate_lux_c100(1000, 900, GAIN_16X, INTEG_402MS)),
    TestCase(1000, 1500, GAIN_16X, INTEG_402MS, calculate_lux_c100(1000, 1500, GAIN_16X, INTEG_402MS)),
    # 101ms, 16x gain
    TestCase(500, 100, GAIN_16X, INTEG_101MS, calculate_lux_c100(500, 100, GAIN_16X, INTEG_101MS)),
    # 402ms, 1x gain
    TestCase(5000, 1000, GAIN_1X, INTEG_402MS, calculate_lux_c100(5000, 1000, GAIN_1X, INTEG_402MS)),
]

if __name__ == "__main__":
    print("TSL2561 Golden Vectors:")
    for i, tc in enumerate(TEST_VECTORS):
        whole = tc.expected_c100 // 100
        frac = tc.expected_c100 % 100
        print(f"Vector {i}: ch0={tc.ch0}, ch1={tc.ch1}, gain={tc.gain}, integ={tc.integ} -> "
              f"{tc.expected_c100} cLux ({whole}.{frac:02d} Lux)")
