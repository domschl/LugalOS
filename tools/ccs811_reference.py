#!/usr/bin/env python3
"""tools/ccs811_reference.py -- Independent reference math for AMS CCS811.

Phase 46, plan/phase46_sensor_framework.md §4.7 & §4.9.
Verifies CCS811 environmental data encoding (ENV_DATA register 0x05)
and algorithm result unpacking (ALG_RESULT_DATA register 0x02).
"""

from typing import NamedTuple


def encode_humidity(rh_cpercent: int) -> tuple[int, int]:
    """Encodes relative humidity in 0.01 %RH into CCS811 1/512 %RH format (2 bytes).

    Datasheet: 50.0% RH = 0x64, 0x00. 48.5% RH = 0x61, 0x00.
    """
    if rh_cpercent < 0:
        rh_cpercent = 0
    if rh_cpercent > 10000:
        rh_cpercent = 10000
    raw = (rh_cpercent * 512 + 50) // 100
    return (raw >> 8) & 0xFF, raw & 0xFF


def encode_temperature(temp_c100: int) -> tuple[int, int]:
    """Encodes temperature in 0.01 °C into CCS811 1/512 °C format with -25°C offset.

    Datasheet: 25.0°C = 0x64, 0x00. 23.5°C = 0x61, 0x00.
    """
    offset_c100 = temp_c100 + 2500
    if offset_c100 < 0:
        offset_c100 = 0
    raw = (offset_c100 * 512 + 50) // 100
    return (raw >> 8) & 0xFF, raw & 0xFF


def encode_env_data(temp_c100: int, rh_cpercent: int) -> bytes:
    """Produces the 4-byte payload for ENV_DATA (reg 0x05)."""
    h_hi, h_lo = encode_humidity(rh_cpercent)
    t_hi, t_lo = encode_temperature(temp_c100)
    return bytes([h_hi, h_lo, t_hi, t_lo])


def decode_alg_result(data: bytes) -> tuple[int, int, int]:
    """Unpacks eCO2 (ppm), TVOC (ppb), and STATUS from ALG_RESULT_DATA (reg 0x02)."""
    if len(data) < 4:
        raise ValueError("ALG_RESULT_DATA must be at least 4 bytes")
    eco2 = (data[0] << 8) | data[1]
    tvoc = (data[2] << 8) | data[3]
    status = data[4] if len(data) > 4 else 0
    return eco2, tvoc, status


class GoldenVector(NamedTuple):
    temp_c100: int
    rh_cpercent: int
    expected_env: bytes


GOLDEN_VECTORS = [
    GoldenVector(2500, 5000, bytes([0x64, 0x00, 0x64, 0x00])),  # 25.0 C, 50.0 %RH (datasheet default)
    GoldenVector(2350, 4850, bytes([0x61, 0x00, 0x61, 0x00])),  # 23.5 C, 48.5 %RH (datasheet example)
    GoldenVector(2000, 4000, bytes([0x50, 0x00, 0x5A, 0x00])),  # 20.0 C, 40.0 %RH
    GoldenVector(0, 0, bytes([0x00, 0x00, 0x32, 0x00])),        # 0.0 C, 0.0 %RH (0°C = 25°C offset -> 50)
    GoldenVector(-2500, 0, bytes([0x00, 0x00, 0x00, 0x00])),    # -25.0 C (minimum offset 0)
    GoldenVector(2680, 5230, bytes([0x68, 0x9A, 0x67, 0x9A])),  # 26.8 C, 52.3 %RH
]

if __name__ == "__main__":
    print("CCS811 Golden Environmental Vectors:")
    for i, gv in enumerate(GOLDEN_VECTORS):
        encoded = encode_env_data(gv.temp_c100, gv.rh_cpercent)
        assert encoded == gv.expected_env, f"Mismatch on vector {i}: got {encoded.hex()}, want {gv.expected_env.hex()}"
        print(f"Vector {i}: T={gv.temp_c100/100:.2f} C, RH={gv.rh_cpercent/100:.2f} % -> {encoded.hex()}")
    print("All golden vectors verified!")
