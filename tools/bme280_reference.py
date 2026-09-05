#!/usr/bin/env python3
"""The BMP280/BME280 compensation formulas, transcribed from the datasheet.

Q4, plan/phase26_mqtt_and_environment_sensors.md §4.2. This exists to be a
*second* implementation of the same integer arithmetic, so that agreement
between it and drivers/bme280.c means something. Run it to regenerate the
vector that `sensor selftest` checks against:

    python3 tools/bme280_reference.py

**What this does and does not prove.** Two transcriptions agreeing shows the
arithmetic was not mistyped -- which is the failure mode these formulas
actually have, being twenty lines of shifts and magic constants where a
misplaced >>12 yields plausible-looking nonsense. It does not prove the
register map is right, because both transcriptions read the same map from the
same page. That claim needs a real part, which is why Q4's hardware exit
criterion compares a reading against a separate thermometer and why a golden
vector captured from the fitted sensor joins this one.

The formulas are Bosch's published integer reference (BME280 datasheet
§4.2.3): temperature in 0.01 C, pressure in Q24.8 pascals, humidity in Q22.10
%RH, with t_fine carrying the temperature into the other two.
"""

from __future__ import annotations


def _s32(v: int) -> int:
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


def compensate(cal: dict, adc_T: int, adc_P: int, adc_H: int) -> tuple[int, int, int]:
    # --- temperature: 0.01 C, and t_fine for the other two ---
    var1 = ((adc_T >> 3) - (cal["T1"] << 1)) * cal["T2"] >> 11
    var2 = ((((adc_T >> 4) - cal["T1"]) * ((adc_T >> 4) - cal["T1"])) >> 12) * cal["T3"] >> 14
    t_fine = _s32(var1 + var2)
    T = (t_fine * 5 + 128) >> 8

    # --- pressure: Q24.8 Pa, 64-bit throughout ---
    var1 = t_fine - 128000
    var2 = var1 * var1 * cal["P6"]
    var2 = var2 + ((var1 * cal["P5"]) << 17)
    var2 = var2 + (cal["P4"] << 35)
    var1 = ((var1 * var1 * cal["P3"]) >> 8) + ((var1 * cal["P2"]) << 12)
    var1 = ((1 << 47) + var1) * cal["P1"] >> 33
    if var1 == 0:
        P = 0
    else:
        p = 1048576 - adc_P
        p = ((p << 31) - var2) * 3125
        # C integer division truncates toward zero; Python floors. They differ
        # for negative numerators, which this expression can produce.
        p = -((-p) // var1) if p < 0 else p // var1
        var1 = (cal["P9"] * (p >> 13) * (p >> 13)) >> 25
        var2 = (cal["P8"] * p) >> 19
        P = ((p + var1 + var2) >> 8) + (cal["P7"] << 4)
        P &= 0xFFFFFFFF

    # --- humidity: Q22.10 %RH ---
    if not cal.get("has_humidity", True):
        return (T, P, 0)
    v = t_fine - 76800
    v = (((((adc_H << 14) - (cal["H4"] << 20) - (cal["H5"] * v)) + 16384) >> 15) *
         (((((((v * cal["H6"]) >> 10) * (((v * cal["H3"]) >> 11) + 32768)) >> 10) +
            2097152) * cal["H2"] + 8192) >> 14))
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * cal["H1"]) >> 4)
    v = max(0, min(v, 419430400))
    H = v >> 12
    return (T, P, H)


# A plausible calibration block and three raw readings. The values are in the
# ranges a real part reports; what matters is that both implementations are
# fed exactly these.
VECTOR_CAL = {
    "T1": 28455, "T2": 26619, "T3": 50,
    "P1": 37356, "P2": -10646, "P3": 3024, "P4": 6821, "P5": -108,
    "P6": -7, "P7": 9900, "P8": -10230, "P9": 4285,
    "H1": 75, "H2": 366, "H3": 0, "H4": 306, "H5": 50, "H6": 30,
    "has_humidity": True,
}
VECTOR_RAW = {"adc_T": 519888, "adc_P": 343040, "adc_H": 32768}


if __name__ == "__main__":
    T, P, H = compensate(VECTOR_CAL, **VECTOR_RAW)
    print("expected results for the selftest vector:")
    print(f"  temperature_c100  = {T}        ({T / 100:.2f} C)")
    print(f"  pressure_pa256    = {P}   ({P / 256 / 100:.2f} hPa)")
    print(f"  humidity_rh1024   = {H}     ({H / 1024:.1f} %RH)")
    print()
    print("C literals for drivers/bme280.c's selftest:")
    print(f"  {T}, {P}u, {H}u")
