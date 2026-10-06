#!/usr/bin/env python3
"""tools/sgp30_reference.py -- Independent reference math for Sensirion SGP30.

Phase 46, plan/phase46_sensor_framework.md §4.8.
Computes CRC-8 (Dallas/Maxim 0x31) checksums and verifies command packets
and absolute humidity conversion without floating point instructions.
"""


def sensirion_crc8(data: bytes) -> int:
    """Computes CRC-8 according to Sensirion specification:
    Polynomial: 0x31 (x^8 + x^5 + x^4 + 1)
    Init: 0xFF
    Final XOR: 0x00
    """
    crc = 0xFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x31) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def calculate_absolute_humidity_8_8(temp_c100: int, rh_cpercent: int) -> int:
    """Computes absolute humidity in fixed-point 8.8 format (1/256 g/m^3)
    from ambient temperature (0.01 °C) and relative humidity (0.01 %RH)
    using pure integer series.
    """
    if rh_cpercent <= 0 or temp_c100 < -4000:
        return 0

    t_m = temp_c100
    # x = (17.62 * T) / (243.12 + T) in Q16
    x_q16 = (1762 * t_m * 65536) // ((24312 + t_m) * 100)

    # exp(x) via Taylor series up to x^6 / 720 in Q16
    e = 65536 + x_q16
    t2 = (x_q16 * x_q16) >> 17  # / 2
    e += t2
    t3 = (t2 * x_q16) // (3 * 65536)  # / 6
    e += t3
    t4 = (t3 * x_q16) // (4 * 65536)  # / 24
    e += t4
    t5 = (t4 * x_q16) // (5 * 65536)  # / 120
    e += t5
    t6 = (t5 * x_q16) // (6 * 65536)  # / 720
    e += t6

    # dv = 216.7 * ( (RH/100) * 6.112 * exp(x) ) / (273.15 + T)
    # Scaled by 256 for 8.8 bit format:
    num = 21670 * 6112 * e * rh_cpercent
    den = (27315 + t_m) * 256 * 10000000
    dv_8_8 = (num + (den // 2)) // den
    if dv_8_8 > 0xFFFF:
        dv_8_8 = 0xFFFF
    return dv_8_8


if __name__ == "__main__":
    print("SGP30 Reference Tests:")
    # Datasheet test vector: CRC(0xBEEF) must be 0x92
    beef_crc = sensirion_crc8(bytes([0xBE, 0xEF]))
    assert beef_crc == 0x92, f"CRC test failed: got 0x{beef_crc:02x}, want 0x92"
    print(f"  CRC(0xBEEF) = 0x{beef_crc:02x} [PASS]")

    # Measure IAQ response verification
    # Example response: eCO2 = 400 (0x0190), TVOC = 0 (0x0000)
    eco2_bytes = bytes([0x01, 0x90])
    eco2_crc = sensirion_crc8(eco2_bytes)
    tvoc_bytes = bytes([0x00, 0x00])
    tvoc_crc = sensirion_crc8(tvoc_bytes)
    print(f"  eCO2=400 (0x0190) -> CRC=0x{eco2_crc:02x}")
    print(f"  TVOC=0   (0x0000) -> CRC=0x{tvoc_crc:02x}")

    # Absolute humidity at 25.0 C, 50.0 %RH:
    ah = calculate_absolute_humidity_8_8(2500, 5000)
    ah_whole = ah >> 8
    ah_frac = (ah & 0xFF) * 1000 // 256
    print(f"  Absolute Humidity at 25.0 C, 50.0 %RH: 0x{ah:04x} = {ah_whole}.{ah_frac:03d} g/m3")
    assert 11000 <= (ah_whole * 1000 + ah_frac) <= 12000, f"AH out of range: {ah_whole}.{ah_frac}"
    print("All SGP30 reference tests passed!")
