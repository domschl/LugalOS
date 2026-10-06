#!/usr/bin/env python3
"""The TSL2591 light sensor Lux compensation formulas.

Phase 46, plan/phase46_sensor_framework.md §4.3 & §4.6. Independent reference
implementation of pure integer arithmetic for ambient light illuminance.

Transcribed from AMS TSL2591 Datasheet & Linux IIO driver (drivers/iio/light/tsl2591.c):
- Dual photodiode: CH0 (broadband visible + IR) and CH1 (IR only)
- Counts Per Lux (CPL) = (ATIME_ms * AGAIN) / 408
- lux = ((CH0 - CH1) * (1 - (CH1 / CH0))) / CPL
- Evaluated in pure integer fixed-point (0.01 Lux, centi-Lux)
"""

from __future__ import annotations


def calculate_lux(ch0: int, ch1: int, gain_mult: int = 25, int_time_ms: int = 100) -> int:
    """Calculates illuminance in centi-Lux (0.01 Lux). Returns 0 on darkness or invalid data."""
    if ch0 == 0 or ch0 <= ch1:
        return 0
    # Guard against saturation (16-bit ADC max is 37888 at 100ms, or 65535)
    if ch0 >= 0xFFFF or ch1 >= 0xFFFF:
        ch0 = 0xFFFF
        if ch1 >= ch0:
            return 0

    cpl = (int_time_ms * gain_mult) // 408
    if cpl == 0:
        cpl = 1

    # millilux = ((ch0 - ch1) * (1000 - (ch1 * 1000 // ch0))) // cpl
    ratio = (ch1 * 1000) // ch0
    lux_m = ((ch0 - ch1) * (1000 - ratio)) // cpl
    # centi-lux (0.01 Lux)
    return lux_m // 10


# Reference vectors for selftest
VECTOR_1 = {"ch0": 2000, "ch1": 500, "gain": 25, "time_ms": 100, "want_clux": 18750}
VECTOR_2 = {"ch0": 5000, "ch1": 1200, "gain": 25, "time_ms": 100, "want_clux": 48133}


if __name__ == "__main__":
    v1 = calculate_lux(VECTOR_1["ch0"], VECTOR_1["ch1"], VECTOR_1["gain"], VECTOR_1["time_ms"])
    v2 = calculate_lux(VECTOR_2["ch0"], VECTOR_2["ch1"], VECTOR_2["gain"], VECTOR_2["time_ms"])
    print("TSL2591 Reference Vectors:")
    print(f"  Vector 1: {v1} centi-lux ({v1 / 100:.2f} Lux) - expected {VECTOR_1['want_clux']}")
    print(f"  Vector 2: {v2} centi-lux ({v2 / 100:.2f} Lux) - expected {VECTOR_2['want_clux']}")
