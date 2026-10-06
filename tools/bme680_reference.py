#!/usr/bin/env python3
"""The BME680 compensation formulas, transcribed from the Bosch datasheet.

Phase 46, plan/phase46_sensor_framework.md §4.3. This exists to be an independent
reference implementation of the pure integer arithmetic, ensuring drivers/bme680.c
is verified without binary blobs and without hardware.

Run it to regenerate and verify the golden reference vector:

    python3 tools/bme680_reference.py

Formulas are transcribed from Bosch Sensortec BME680 Datasheet BST-BME680-DS001-09:
- Temperature (§3.3.1): 0.01 °C, and t_fine feeds pressure, humidity, heater target
- Pressure (§3.3.2): 1 Pa
- Humidity (§3.3.3): 0.01 %RH (scaled from 0.001 %RH)
- Gas Resistance (§3.4.1): 1 Ohm (using lookup tables for gas range)
- Target Heater Resistance (§3.3.5): register code for res_heat_x
- Gas Wait Duration (§5.3.3.3): register code for gas_wait_x
"""

from __future__ import annotations


def _c_div(a: int, b: int) -> int:
    """C integer division truncates towards zero (unlike Python floor division)."""
    if (a < 0) ^ (b < 0):
        return -((-a if a < 0 else a) // (-b if b < 0 else b))
    else:
        return a // b


def calc_temperature(temp_adc: int, cal: dict[str, int]) -> tuple[int, int]:
    """Compensates raw temperature ADC to 0.01 °C and t_fine."""
    var1 = (temp_adc >> 3) - (cal["par_t1"] << 1)
    var2 = (var1 * cal["par_t2"]) >> 11
    var3 = ((((var1 >> 1) * (var1 >> 1)) >> 12) * (cal["par_t3"] << 4)) >> 14
    t_fine = var2 + var3
    calc_temp = ((t_fine * 5) + 128) >> 8
    return calc_temp, t_fine


def calc_pressure(pres_adc: int, t_fine: int, cal: dict[str, int]) -> int:
    """Compensates raw pressure ADC to 1 Pa."""
    var1 = (t_fine >> 1) - 64000
    var2 = ((((var1 >> 2) * (var1 >> 2)) >> 11) * cal["par_p6"]) >> 2
    var2 = var2 + ((var1 * cal["par_p5"]) << 1)
    var2 = (var2 >> 2) + (cal["par_p4"] << 16)
    var1 = (((((var1 >> 2) * (var1 >> 2)) >> 13) * (cal["par_p3"] << 5)) >> 3) + ((cal["par_p2"] * var1) >> 1)
    var1 = var1 >> 18
    var1 = ((32768 + var1) * cal["par_p1"]) >> 15
    if var1 == 0:
        return 0
    press_comp = 1048576 - pres_adc
    press_comp = (press_comp - (var2 >> 12)) * 3125
    if press_comp >= 0x40000000:
        press_comp = _c_div(press_comp, var1) << 1
    else:
        press_comp = _c_div(press_comp << 1, var1)
    var1 = (cal["par_p9"] * (((press_comp >> 3) * (press_comp >> 3)) >> 13)) >> 12
    var2 = ((press_comp >> 2) * cal["par_p8"]) >> 13
    var3 = ((press_comp >> 8) * (press_comp >> 8) * (press_comp >> 8) * cal["par_p10"]) >> 17
    press_comp = press_comp + ((var1 + var2 + var3 + (cal["par_p7"] << 7)) >> 4)
    return press_comp


def calc_humidity(hum_adc: int, t_fine: int, cal: dict[str, int]) -> int:
    """Compensates raw humidity ADC to 0.01 %RH (scaled from internal 0.001 %RH)."""
    temp_scaled = ((t_fine * 5) + 128) >> 8
    var1 = hum_adc - (cal["par_h1"] << 4) - _c_div(_c_div(temp_scaled * cal["par_h3"], 100), 2)
    var2 = (
        cal["par_h2"]
        * (
            _c_div(temp_scaled * cal["par_h4"], 100)
            + _c_div(_c_div(temp_scaled * _c_div(temp_scaled * cal["par_h5"], 100), 64), 100)
            + (1 << 14)
        )
    ) >> 10
    var3 = var1 * var2
    var4 = ((cal["par_h6"] << 7) + _c_div(temp_scaled * cal["par_h7"], 100)) >> 4
    var5 = ((var3 >> 14) * (var3 >> 14)) >> 10
    var6 = (var4 * var5) >> 1
    calc_hum = (((var3 + var6) >> 10) * 1000) >> 12
    calc_hum = max(0, min(100000, calc_hum))
    # Convert 0.001 %RH to 0.01 %RH
    return (calc_hum + 5) // 10


LOOKUP_TABLE1 = [
    2147483647, 2147483647, 2147483647, 2147483647, 2147483647,
    2126008810, 2147483647, 2130303777, 2147483647, 2147483647,
    2143188679, 2136746228, 2147483647, 2126008810, 2147483647,
    2147483647,
]

LOOKUP_TABLE2 = [
    4096000000, 2048000000, 1024000000, 512000000, 255744255,
    127110228, 64000000, 32258064, 16016016, 8000000,
    4000000, 2000000, 1000000, 500000, 250000, 125000,
]


def calc_gas_resistance(gas_adc: int, gas_range: int, cal: dict[str, int]) -> int:
    """Compensates raw gas ADC and range to resistance in Ohms."""
    if gas_range > 15:
        return 0
    var1 = ((1340 + (5 * cal["range_sw_err"])) * LOOKUP_TABLE1[gas_range]) >> 16
    var2 = ((gas_adc << 15) - 16777216) + var1
    if var2 == 0:
        return 0
    var3 = (LOOKUP_TABLE2[gas_range] * var1) >> 9
    calc_gas_res = _c_div(var3 + (var2 >> 1), var2)
    return calc_gas_res


def calc_res_heat(target_temp: int, amb_temp: int, cal: dict[str, int]) -> int:
    """Calculates res_heat_x register code for target heater temp (e.g. 320 °C)."""
    if target_temp > 400:
        target_temp = 400
    var1 = (_c_div(amb_temp * cal["par_gh3"], 1000)) * 256
    var2 = (cal["par_gh1"] + 784) * _c_div(
        (_c_div((cal["par_gh2"] + 154009) * target_temp * 5, 100) + 3276800), 10
    )
    var3 = var1 + (var2 >> 1)
    var4 = _c_div(var3, (cal["res_heat_range"] + 4))
    var5 = (131 * cal["res_heat_val"]) + 65536
    res_heat_x100 = (_c_div(var4, var5) - 250) * 34
    res_heat = (res_heat_x100 + 50) // 100
    return max(0, min(255, res_heat))


def calc_gas_wait(dur_ms: int) -> int:
    """Calculates gas_wait_x register code for duration in milliseconds."""
    if dur_ms >= 0xFC0:
        return 0xFF
    factor = 0
    d = dur_ms
    while d > 0x3F:
        d //= 4
        factor += 1
    return d + (factor * 64)


# Reference calibration vector from representative silicon
VECTOR_CAL = {
    "par_t1": 26182, "par_t2": 26344, "par_t3": 3,
    "par_p1": 36171, "par_p2": -10502, "par_p3": 30, "par_p4": 6554,
    "par_p5": 11, "par_p6": 30, "par_p7": 45, "par_p8": -1141,
    "par_p9": -812, "par_p10": 30,
    "par_h1": 735, "par_h2": 1018, "par_h3": 0, "par_h4": 45,
    "par_h5": 20, "par_h6": 120, "par_h7": -100,
    "par_gh1": -16, "par_gh2": -10321, "par_gh3": 18,
    "res_heat_val": 50, "res_heat_range": 1, "range_sw_err": 0,
}

VECTOR_RAW = {
    "temp_adc": 500000,
    "pres_adc": 350000,
    "hum_adc": 20000,
    "gas_adc": 500,
    "gas_range": 0,
}


def compensate_all(cal: dict[str, int], raw: dict[str, int]) -> tuple[int, int, int, int]:
    t, t_fine = calc_temperature(raw["temp_adc"], cal)
    p = calc_pressure(raw["pres_adc"], t_fine, cal)
    h = calc_humidity(raw["hum_adc"], t_fine, cal)
    g = calc_gas_resistance(raw["gas_adc"], raw["gas_range"], cal)
    return t, p, h, g


if __name__ == "__main__":
    t, p, h, g = compensate_all(VECTOR_CAL, VECTOR_RAW)
    rh = calc_res_heat(320, 25, VECTOR_CAL)
    gw = calc_gas_wait(150)
    print("BME680 Golden Reference Vector Results:")
    print(f"  temperature_c100  = {t} ({t / 100:.2f} C)")
    print(f"  pressure_pa       = {p} ({p / 100:.2f} hPa)")
    print(f"  humidity_cpercent = {h} ({h / 100:.2f} %RH)")
    print(f"  gas_res_ohm       = {g} ({g / 1000:.2f} kOhm)")
    print(f"  res_heat_0 (320C) = {rh} (0x{rh:02x})")
    print(f"  gas_wait_0 (150ms)= {gw} (0x{gw:02x})")
