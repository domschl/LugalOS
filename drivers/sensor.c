#include "drivers/sensor.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include <string.h>

/* Uniform sensor channel descriptors (Phase 46 §7.2) */
static const sensor_chan_desc_t s_descriptors[SENSOR_CHAN_MAX] = {
    [SENSOR_CHAN_TEMP]         = { SENSOR_CHAN_TEMP,         "temperature",    "C",    2, 100 },
    [SENSOR_CHAN_PRESSURE]     = { SENSOR_CHAN_PRESSURE,     "pressure",       "hPa",  2, 100 },
    [SENSOR_CHAN_HUMIDITY]     = { SENSOR_CHAN_HUMIDITY,     "humidity",       "%RH",  2, 100 },
    [SENSOR_CHAN_LUX]          = { SENSOR_CHAN_LUX,          "lux",            "Lux",  2, 100 },
    [SENSOR_CHAN_ECO2]         = { SENSOR_CHAN_ECO2,         "eco2",           "ppm",  0, 1   },
    [SENSOR_CHAN_TVOC]         = { SENSOR_CHAN_TVOC,         "tvoc",           "ppb",  0, 1   },
    [SENSOR_CHAN_GAS_RES]      = { SENSOR_CHAN_GAS_RES,      "gas_resistance", "Ohm",  0, 1   },
    [SENSOR_CHAN_CO]           = { SENSOR_CHAN_CO,           "co",             "ppm",  2, 100 },
    [SENSOR_CHAN_NO2]          = { SENSOR_CHAN_NO2,          "no2",            "ppm",  2, 100 },
    [SENSOR_CHAN_NH3]          = { SENSOR_CHAN_NH3,          "nh3",            "ppm",  2, 100 },
    [SENSOR_CHAN_CO2]          = { SENSOR_CHAN_CO2,          "co2",            "ppm",  0, 1   },
    [SENSOR_CHAN_PRESSURE_MSL] = { SENSOR_CHAN_PRESSURE_MSL, "pressure_msl",   "hPa",  2, 100 },
    [SENSOR_CHAN_AH]           = { SENSOR_CHAN_AH,           "abs_humidity",   "g/m3", 2, 100 },
    [SENSOR_CHAN_DEW_POINT]    = { SENSOR_CHAN_DEW_POINT,    "dew_point",      "C",    2, 100 },
    [SENSOR_CHAN_IAQ]          = { SENSOR_CHAN_IAQ,          "iaq",            "IAQ",  0, 1   },
};

const sensor_chan_desc_t *sensor_chan_desc(sensor_chan_t chan) {
    if (chan >= SENSOR_CHAN_MAX) return NULL;
    return &s_descriptors[chan];
}

const char *sensor_tier_name(sensor_tier_t tier) {
    switch (tier) {
    case SENSOR_TIER_HW:       return "hw";
    case SENSOR_TIER_INFERRED: return "inferred";
    case SENSOR_TIER_FUSED:    return "fused";
    default:                   return "unknown";
    }
}

const char *sensor_chan_name(sensor_chan_t chan) {
    if (chan >= SENSOR_CHAN_MAX) return "unknown";
    return s_descriptors[chan].name;
}

const char *sensor_chan_unit(sensor_chan_t chan) {
    if (chan >= SENSOR_CHAN_MAX) return "";
    return s_descriptors[chan].unit_symbol;
}

uint8_t sensor_chan_decimals(sensor_chan_t chan) {
    if (chan >= SENSOR_CHAN_MAX) return 0;
    return s_descriptors[chan].decimals;
}

int32_t sensor_chan_scale_div(sensor_chan_t chan) {
    if (chan >= SENSOR_CHAN_MAX) return 1;
    return s_descriptors[chan].scale_div;
}

size_t sensor_format_channel(sensor_chan_t chan, int32_t raw_val, char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    const sensor_chan_desc_t *desc = sensor_chan_desc(chan);
    if (!desc) {
        return (size_t)ksnprintf(buf, cap, "%ld", (long)raw_val);
    }

    if (desc->decimals == 0) {
        if (chan == SENSOR_CHAN_GAS_RES && raw_val >= 1000) {
            /* Format as kOhm for readability */
            int32_t whole = raw_val / 1000;
            int32_t frac = (raw_val % 1000) / 10;
            return (size_t)ksnprintf(buf, cap, "%ld.%02ld kOhm", (long)whole, (long)frac);
        }
        return (size_t)ksnprintf(buf, cap, "%ld %s", (long)raw_val, desc->unit_symbol);
    } else if (desc->decimals == 2 && desc->scale_div == 100) {
        bool neg = raw_val < 0;
        int32_t abs_val = neg ? -raw_val : raw_val;
        int32_t whole = abs_val / 100;
        int32_t frac = abs_val % 100;
        return (size_t)ksnprintf(buf, cap, "%s%ld.%02ld %s",
                                 neg ? "-" : "", (long)whole, (long)frac, desc->unit_symbol);
    }

    return (size_t)ksnprintf(buf, cap, "%ld %s", (long)raw_val, desc->unit_symbol);
}

/* --- Derived Metrics Calculations (Zero-Float Invariant) --- */

/* Sea-level pressure (phase 40 item 11, phase 46 §7.3):
 * p0 / p = (1 - x) ** -5.257, x = L h / (T + L h + 273.15), L = 0.0065 K/m.
 * Evaluated as exp(5.257 * -ln(1 - x)) with series in Q30 arithmetic. */
int32_t sensor_calc_sea_level_pa(int32_t pa, int32_t t_c100, int32_t alt_m) {
    if (alt_m == 0 || pa <= 0) return pa;
    int64_t num = 65LL * alt_m;
    int64_t den = 100LL * t_c100 + 2731500LL + num;
    if (den <= 0) return pa;
    const int64_t ONE = 1LL << 30;
    int64_t x = (num * ONE) / den;

    int64_t s = 0, xk = x;
    for (int k = 1; k <= 7; k++) {
        s += xk / k;
        xk = (xk * x) / ONE;
    }
    int64_t y = (s * 5257) / 1000;

    int64_t e = ONE, term = ONE;
    for (int k = 1; k <= 9; k++) {
        term = (term * y) / ONE / k;
        e += term;
    }
    return (int32_t)(((int64_t)pa * e + (ONE / 2)) / ONE);
}

/* Absolute humidity in centi-g/m³ (0.01 g/m³, e.g. 1148 is 11.48 g/m³).
 * Evaluates Magnus-Tetens formula via Q20 fixed-point series (zero-float):
 * AH = 216.7 * (RH/100 * 6.112 * exp(x)) / (T + 273.15) */
int32_t sensor_calc_abs_humidity_c100(int32_t t_c100, int32_t rh_c100) {
    if (rh_c100 <= 0 || t_c100 < -4000) return 0;
    int32_t t_m = t_c100;
    const int64_t ONE = 1LL << 20;

    /* x = 17.62 * T / (243.12 + T) in Q20 arithmetic */
    int64_t num_x = 1762LL * t_m * ONE;
    int64_t den_x = ((int64_t)24312 + t_m) * 100LL;
    if (den_x <= 0) return 0;
    int64_t x = num_x / den_x;

    /* e = exp(x) in Q20 series expansion */
    int64_t e = ONE, term = ONE;
    for (int k = 1; k <= 7; k++) {
        term = (term * x) / ONE / k;
        e += term;
    }

    /* AH_c100 = 132447 * rh_c100 * e / ((T + 273.15) * 100 * ONE)
     * Numerator peaks around ~2e16, strictly within int64_t capacity (9.22e18). */
    int64_t num = 132447LL * rh_c100 * e;
    int64_t den = ((int64_t)t_m + 27315LL) * ONE * 100LL;
    if (den <= 0) return 0;
    return (int32_t)((num + (den / 2)) / den);
}

/* Dew point in centi-degrees Celsius (0.01 °C, e.g. 930 is 9.30 °C).
 * Uses August-Roche-Magnus approximation with temperature-scaled correction:
 * T_dew ≈ T - (100 - RH) / 5 * (1 + 0.035 * T / 10) */
int32_t sensor_calc_dew_point_c100(int32_t t_c100, int32_t rh_c100) {
    if (rh_c100 <= 0) return t_c100;
    if (rh_c100 >= 10000) return t_c100;

    int64_t diff_rh = 10000LL - rh_c100; /* centi-%RH deficit */
    /* Factor: 1000 base at 0 C, +3.5% per 10 C */
    int64_t factor = 1000LL + (int64_t)t_c100 * 35LL / 1000LL;
    if (factor < 700) factor = 700;
    int64_t depression = (diff_rh * factor) / 5000LL;
    return t_c100 - (int32_t)depression;
}

/* Open-source BME680 IAQ (0-500 index) from raw gas resistance, temperature, and humidity.
 * Eliminates proprietary Bosch BSEC binary blob (Phase 46 §7.4). */
int32_t sensor_calc_bme680_iaq(int32_t gas_res_ohm, int32_t t_c100, int32_t rh_c100, int32_t *inout_r_base) {
    (void)t_c100;
    if (gas_res_ohm <= 0) return 0;

    /* 1. Humidity normalization to reference 40% RH (4000 centi-%RH):
     * ±0.2% per %RH: 2 per 1000 */
    int64_t r_comp = gas_res_ohm + (int64_t)gas_res_ohm * 2LL * (rh_c100 - 4000) / 100000LL;
    if (r_comp <= 0) r_comp = gas_res_ohm;

    /* 2. Adaptive baseline tracking */
    if (!inout_r_base || *inout_r_base <= 0) {
        if (inout_r_base) *inout_r_base = (int32_t)r_comp;
        return 25; /* Initial clean baseline reading */
    }

    if (r_comp > *inout_r_base) {
        /* Faster ascent to clean air */
        *inout_r_base += (int32_t)((r_comp - *inout_r_base) >> 3);
    } else {
        /* Very slow decay to prevent baseline poisoning from indoor VOC events */
        *inout_r_base -= (int32_t)((*inout_r_base - r_comp) >> 12);
        if (*inout_r_base <= 0) *inout_r_base = 1;
    }

    /* 3. Scaled ratio and IAQ 0-500 calculation */
    int64_t base = *inout_r_base;
    int64_t ratio_q10 = (r_comp << 10) / base;
    if (ratio_q10 > 1024) ratio_q10 = 1024;
    if (ratio_q10 < 0) ratio_q10 = 0;

    int32_t iaq = (int32_t)(500 - (ratio_q10 * 475) / 1024);
    if (iaq < 0) iaq = 0;
    if (iaq > 500) iaq = 500;
    return iaq;
}

uint32_t sensor_derived_selftest(bool report) {
    uint32_t failed = 0;

    /* 1. Sea level pressure against golden vectors */
    static const struct { int32_t pa, t_c100, alt_m, want; } msl[] = {
        { 101325, 1500, 0, 101325 },
        { 96369, 2050, 520, 102345 },
        { 89875, -1000, 1000, 102176 },
        { 70121, -2000, 3000, 103578 },
        { 100000, 3000, -100, 98878 },
    };
    for (unsigned i = 0; i < sizeof(msl) / sizeof(msl[0]); i++) {
        int32_t got = sensor_calc_sea_level_pa(msl[i].pa, msl[i].t_c100, msl[i].alt_m);
        int32_t d = got - msl[i].want;
        if (d < -1 || d > 1) {
            failed++;
            if (report) cprintf("  msl test %u failed: got %ld, want %ld\n", i, (long)got, (long)msl[i].want);
        }
    }

    /* 2. Absolute humidity test at 25.0 C, 50.0 %RH -> 11.46 g/m³ (1146 centi-g/m³) */
    int32_t ah = sensor_calc_abs_humidity_c100(2500, 5000);
    if (ah < 1140 || ah > 1155) {
        failed++;
        if (report) cprintf("  abs humidity test failed: got %ld, want ~1146\n", (long)ah);
    }

    /* 3. Dew point test at 20.0 C, 50.0 %RH -> ~9.3 °C (930 centi-°C) */
    int32_t dp = sensor_calc_dew_point_c100(2000, 5000);
    if (dp < 900 || dp > 960) {
        failed++;
        if (report) cprintf("  dew point test failed: got %ld, want ~930\n", (long)dp);
    }

    /* 4. Open-source BME680 IAQ calculation test */
    int32_t r_base = 100000; /* 100 kOhm baseline */
    int32_t iaq_clean = sensor_calc_bme680_iaq(100000, 2500, 4000, &r_base);
    if (iaq_clean < 20 || iaq_clean > 30) {
        failed++;
        if (report) cprintf("  iaq clean test failed: got %ld, want 25\n", (long)iaq_clean);
    }

    int32_t iaq_polluted = sensor_calc_bme680_iaq(50000, 2500, 4000, &r_base);
    if (iaq_polluted < 250 || iaq_polluted > 280) {
        failed++;
        if (report) cprintf("  iaq polluted test failed: got %ld, want ~262\n", (long)iaq_polluted);
    }

    if (report) {
        cprintf("sensor_derived selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1u ? "" : "s");
    }
    return failed;
}
