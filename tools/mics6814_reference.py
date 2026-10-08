#!/usr/bin/env python3
"""tools/mics6814_reference.py -- Independent reference math for Grove Multichannel Gas Sensor (MiCS-6814).

Phase 46, plan/phase46_sensor_framework.md.
Computes gas concentration (CO, NO2, NH3) from sensor resistance ratios
using pure fixed-point arithmetic without floating point instructions.
"""


def fixed_ln(val_million: int) -> int:
    """Fixed-point natural logarithm with scale 10^6.
    Computes ln(val_million / 10^6) * 10^6 via range reduction and Taylor series.
    """
    if val_million <= 0:
        return -14000000
    val = val_million
    k = 0
    while val >= 2000000:
        val >>= 1
        k += 1
    while val < 1000000:
        val <<= 1
        k -= 1
    num = val - 1000000
    den = val + 1000000
    z = (num * 1000000) // den
    z2 = (z * z) // 1000000
    t1 = z
    t3 = (t1 * z2) // 1000000
    t5 = (t3 * z2) // 1000000
    t7 = (t5 * z2) // 1000000
    t9 = (t7 * z2) // 1000000
    ln_m = 2 * (t1 + t3 // 3 + t5 // 5 + t7 // 7 + t9 // 9)
    return k * 693147 + ln_m


def fixed_exp(u_million: int) -> int:
    """Fixed-point exponential with scale 10^6.
    Computes exp(u_million / 10^6) * 10^6.
    """
    if u_million < -14000000:
        return 0
    if u_million > 14000000:
        u_million = 14000000
    ln2 = 693147
    k = u_million // ln2
    r = u_million % ln2
    if r < 0:
        r += ln2
        k -= 1
    t0 = 1000000
    t1 = r
    t2 = (t1 * r) // 2000000
    t3 = (t2 * r) // 3000000
    t4 = (t3 * r) // 4000000
    t5 = (t4 * r) // 5000000
    t6 = (t5 * r) // 6000000
    t7 = (t6 * r) // 7000000
    e_r = t0 + t1 + t2 + t3 + t4 + t5 + t6 + t7
    val = (e_r << k) if k >= 0 else (e_r >> (-k))
    return val


def calc_gas_fixed(ratio_q10: int, gas_type: str) -> int:
    """Calculates gas concentration in 0.01 ppm (centi-ppm) from ratio Q10 (where 1024 = 1.0).
    Curve fits from MiCS-6814 / Seeed driver:
      CO:  c = 4.385 * ratio^(-1.179) ppm
      NO2: c = (1/6.855) * ratio^(1.007) ppm
      NH3: c = (1/1.47)  * ratio^(-1.670) ppm
    """
    if ratio_q10 <= 0:
        return 0
    r_million = (ratio_q10 * 1000000) // 1024
    ln_r = fixed_ln(r_million)

    if gas_type == "CO":
        u = (ln_r * -1179) // 1000
        A = 438500  # 4.385 * 100 * 1000 (milli-centi-ppm)
    elif gas_type == "NO2":
        u = (ln_r * 1007) // 1000
        A = 14588  # (1/6.855) * 100 * 1000
    elif gas_type == "NH3":
        u = (ln_r * -1670) // 1000
        A = 68027  # (1/1.47) * 100 * 1000
    else:
        return 0

    e_u = fixed_exp(u)
    return (A * e_u + 500000000) // 1000000000


GOLDEN_VECTORS = [
    # (ratio_q10, expected_co_cppm, expected_no2_cppm, expected_nh3_cppm)
    (512, 993, 7, 216),    # ratio = 0.50
    (1024, 439, 15, 68),   # ratio = 1.00
    (1536, 272, 22, 35),   # ratio = 1.50
    (2048, 194, 29, 21),   # ratio = 2.00
]

if __name__ == "__main__":
    print("MiCS-6814 Reference Verification:")
    for r_q10, want_co, want_no2, want_nh3 in GOLDEN_VECTORS:
        co = calc_gas_fixed(r_q10, "CO")
        no2 = calc_gas_fixed(r_q10, "NO2")
        nh3 = calc_gas_fixed(r_q10, "NH3")
        print(f"  ratio_q10={r_q10:4d} (r={r_q10/1024:.2f}): CO={co:4d} c-ppm, NO2={no2:3d} c-ppm, NH3={nh3:4d} c-ppm")
        assert co == want_co, f"CO mismatch: {co} != {want_co}"
        assert no2 == want_no2, f"NO2 mismatch: {no2} != {want_no2}"
        assert nh3 == want_nh3, f"NH3 mismatch: {nh3} != {want_nh3}"
    print("All golden vectors passed!")
