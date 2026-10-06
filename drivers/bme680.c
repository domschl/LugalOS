#include "drivers/bme680.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <string.h>

/* Bosch BME680 Environmental and MOX Gas Sensor Driver.
 *
 * Implements pure integer compensation math directly from Bosch Datasheet
 * BST-BME680-DS001-09. Strictly zero binary blobs.
 */

#define REG_STATUS          0x73u
#define REG_RESET           0xE0u
#define REG_CHIP_ID         0xD0u
#define REG_CONFIG          0x75u
#define REG_CTRL_MEAS       0x74u
#define REG_CTRL_HUM        0x72u
#define REG_CTRL_GAS_1      0x71u
#define REG_CTRL_GAS_0      0x70u
#define REG_GAS_WAIT_0      0x64u
#define REG_RES_HEAT_0      0x5Au
#define REG_IDAC_HEAT_0     0x50u
#define REG_MEAS_STAT_0     0x1Du
#define REG_PRESS_MSB       0x1Fu   /* 0x1F..0x26: press, temp, hum */
#define REG_GAS_R_MSB       0x2Au   /* 0x2A..0x2B: gas */

#define REG_COEFF_1         0x8Au   /* 23 bytes: 0x8A..0xA0 */
#define REG_COEFF_2         0xE1u   /* 14 bytes: 0xE1..0xEE */
#define REG_COEFF_3         0x00u   /* 5 bytes: 0x00..0x04 */

#define RESET_CMD           0xB6u

#define MEAS_STAT_NEW_DATA  (1u << 7)
#define MEAS_STAT_GAS_MEAS  (1u << 6)
#define MEAS_STAT_MEASURING (1u << 5)

/* Oversampling x1 for T, P, H; forced mode */
#define OSRS_X1             0x01u
#define CTRL_MEAS_FORCED    ((OSRS_X1 << 5) | (OSRS_X1 << 2) | 0x01u)
#define CTRL_MEAS_SLEEP     ((OSRS_X1 << 5) | (OSRS_X1 << 2) | 0x00u)

static const uint32_t LOOKUP_TABLE1[16] = {
    2147483647u, 2147483647u, 2147483647u, 2147483647u, 2147483647u,
    2126008810u, 2147483647u, 2130303777u, 2147483647u, 2147483647u,
    2143188679u, 2136746228u, 2147483647u, 2126008810u, 2147483647u,
    2147483647u
};

static const uint32_t LOOKUP_TABLE2[16] = {
    4096000000u, 2048000000u, 1024000000u, 512000000u, 255744255u,
    127110228u, 64000000u, 32258064u, 16016016u, 8000000u,
    4000000u, 2000000u, 1000000u, 500000u, 250000u, 125000u
};

static struct {
    bool             detected;
    uint8_t          addr;
    bme680_calib_t   cal;

    bme680_reading_t last;
    uint64_t         last_ms;
    bool             have_last;

    uint32_t         read_count;
    uint32_t         fail_count;
    const char      *last_fail;
} g;

static bool rd(uint8_t reg, uint8_t *dst, uint32_t len) {
    return i2c_reg_read_bytes(g.addr, reg, dst, len);
}

static bool wr(uint8_t reg, uint8_t value) {
    return i2c_reg_write_u8(g.addr, reg, value);
}

static bool fail(const char *where) {
    g.last_fail = where;
    g.fail_count++;
    return false;
}

bool        bme680_is_detected(void)  { return g.detected; }
uint8_t     bme680_address(void)      { return g.addr; }
const char *bme680_part_name(void)    { return "bme680"; }
const bme680_calib_t *bme680_calibration(void) { return &g.cal; }

uint32_t    bme680_read_count(void)   { return g.read_count; }
uint32_t    bme680_fail_count(void)   { return g.fail_count; }
const char *bme680_last_failure(void) { return g.last_fail; }

bool bme680_cached(bme680_reading_t *out, uint32_t *age_s) {
    if (!out || !g.have_last) return false;
    *out = g.last;
    if (age_s) {
        uint64_t now = time_get_ms();
        *age_s = (now >= g.last_ms) ? (uint32_t)((now - g.last_ms) / 1000u) : 0u;
    }
    return true;
}

uint8_t bme680_calc_res_heat(uint16_t target_temp, int16_t amb_temp, const bme680_calib_t *cal) {
    if (!cal) return 0;
    if (target_temp > 400u) target_temp = 400u;

    int32_t var1 = (((int32_t)amb_temp * (int32_t)cal->par_gh3) / 1000) * 256;
    int32_t var2 = ((int32_t)cal->par_gh1 + 784) *
                   ((((((int32_t)cal->par_gh2 + 154009) * (int32_t)target_temp * 5) / 100) + 3276800) / 10);
    int32_t var3 = var1 + (var2 / 2);
    int32_t var4 = var3 / ((int32_t)cal->res_heat_range + 4);
    int32_t var5 = 131 * (int32_t)cal->res_heat_val + 65536;
    int32_t res_heat_x100 = ((var4 / var5) - 250) * 34;
    int32_t res_heat = (res_heat_x100 + 50) / 100;
    if (res_heat < 0) res_heat = 0;
    if (res_heat > 255) res_heat = 255;
    return (uint8_t)res_heat;
}

uint8_t bme680_calc_gas_wait(uint16_t dur_ms) {
    if (dur_ms >= 0xFC0u) return 0xFFu;
    uint8_t factor = 0;
    while (dur_ms > 0x3Fu) {
        dur_ms /= 4u;
        factor++;
    }
    return (uint8_t)(dur_ms + (factor * 64u));
}

static bool read_calibration(void) {
    uint8_t b1[23];
    uint8_t b2[14];
    uint8_t b3[5];

    if (!rd(REG_COEFF_1, b1, 23u)) return false;
    if (!rd(REG_COEFF_2, b2, 14u)) return false;
    if (!rd(REG_COEFF_3, b3, 5u))  return false;

    /* Block 1: 0x8A..0xA0 */
    g.cal.par_t2 = (int16_t)((uint16_t)b1[1] << 8 | b1[0]);
    g.cal.par_t3 = (int8_t)b1[2];
    g.cal.par_p1 = (uint16_t)((uint16_t)b1[5] << 8 | b1[4]);
    g.cal.par_p2 = (int16_t)((uint16_t)b1[7] << 8 | b1[6]);
    g.cal.par_p3 = (int8_t)b1[8];
    g.cal.par_p4 = (int16_t)((uint16_t)b1[11] << 8 | b1[10]);
    g.cal.par_p5 = (int16_t)((uint16_t)b1[13] << 8 | b1[12]);
    g.cal.par_p7 = (int8_t)b1[14];
    g.cal.par_p6 = (int8_t)b1[15];
    g.cal.par_p8 = (int16_t)((uint16_t)b1[19] << 8 | b1[18]);
    g.cal.par_p9 = (int16_t)((uint16_t)b1[21] << 8 | b1[20]);
    g.cal.par_p10 = (uint8_t)b1[22];

    /* Block 2: 0xE1..0xEE */
    g.cal.par_h2 = (uint16_t)(((uint16_t)b2[0] << 4) | (b2[1] >> 4));
    g.cal.par_h1 = (uint16_t)(((uint16_t)b2[2] << 4) | (b2[1] & 0x0Fu));
    g.cal.par_h3 = (int8_t)b2[3];
    g.cal.par_h4 = (int8_t)b2[4];
    g.cal.par_h5 = (int8_t)b2[5];
    g.cal.par_h6 = (uint8_t)b2[6];
    g.cal.par_h7 = (int8_t)b2[7];
    g.cal.par_t1 = (uint16_t)((uint16_t)b2[9] << 8 | b2[8]);
    g.cal.par_gh2 = (int16_t)((uint16_t)b2[11] << 8 | b2[10]);
    g.cal.par_gh1 = (int8_t)b2[12];
    g.cal.par_gh3 = (int8_t)b2[13];

    /* Block 3: 0x00..0x04 */
    g.cal.res_heat_val = (int8_t)b3[0];
    g.cal.res_heat_range = (b3[2] >> 4) & 0x03u;
    g.cal.range_sw_err = ((int8_t)(b3[4] & 0xF0u)) / 16;

    return true;
}

bool bme680_init(void) {
    static const uint8_t ADDRS[] = { BME680_ADDR_LOW, BME680_ADDR_HIGH };
    for (unsigned i = 0; i < sizeof(ADDRS) / sizeof(ADDRS[0]); i++) {
        g.addr = ADDRS[i];
        uint8_t chip_id = 0;
        if (!rd(REG_CHIP_ID, &chip_id, 1u)) continue;
        if (chip_id != BME680_CHIP_ID) continue;

        /* Soft reset */
        (void)wr(REG_RESET, RESET_CMD);
        uint64_t until = time_get_ms() + 10u;
        while (time_get_ms() < until) sched_yield();

        if (!read_calibration()) {
            printk("[BME680] Found BME680 at 0x%02x but could not read calibration.\n", g.addr);
            continue;
        }

        /* Configure heater profile 0: 320 °C target for 150 ms */
        uint8_t rh = bme680_calc_res_heat(320u, 25, &g.cal);
        uint8_t gw = bme680_calc_gas_wait(150u);
        (void)wr(REG_RES_HEAT_0, rh);
        (void)wr(REG_GAS_WAIT_0, gw);

        /* Enable gas conversion on profile 0 (run_gas = 1, nb_conv = 0) */
        (void)wr(REG_CTRL_GAS_1, 0x10u);

        /* Set humidity oversampling x1 */
        (void)wr(REG_CTRL_HUM, OSRS_X1);

        /* IIR filter off */
        (void)wr(REG_CONFIG, 0x00u);

        /* Enter sleep mode until triggered */
        (void)wr(REG_CTRL_MEAS, CTRL_MEAS_SLEEP);

        g.detected = true;
        printk("[BME680] BME680 at 0x%02x on the shared I2C bus.\n", g.addr);
        return true;
    }

    g.addr = 0;
    g.detected = false;
    return false;
}

void bme680_compensate(const bme680_calib_t *cal,
                       int32_t raw_temp, int32_t raw_press,
                       int32_t raw_hum, int32_t raw_gas,
                       uint8_t gas_range, bme680_reading_t *out) {
    if (!cal || !out) return;
    memset(out, 0, sizeof(*out));

    /* --- Temperature (0.01 °C) and t_fine --- */
    int32_t var1 = (raw_temp >> 3) - ((int32_t)cal->par_t1 << 1);
    int32_t var2 = (var1 * (int32_t)cal->par_t2) >> 11;
    int32_t var3 = ((((var1 >> 1) * (var1 >> 1)) >> 12) * ((int32_t)cal->par_t3 << 4)) >> 14;
    int32_t t_fine = var2 + var3;
    out->temperature_c100 = ((t_fine * 5) + 128) >> 8;

    /* --- Pressure (1 Pa) --- */
    int32_t p_var1 = (t_fine >> 1) - 64000;
    int32_t p_var2 = ((((p_var1 >> 2) * (p_var1 >> 2)) >> 11) * (int32_t)cal->par_p6) >> 2;
    p_var2 = p_var2 + ((p_var1 * (int32_t)cal->par_p5) << 1);
    p_var2 = (p_var2 >> 2) + ((int32_t)cal->par_p4 << 16);
    p_var1 = (((((p_var1 >> 2) * (p_var1 >> 2)) >> 13) * ((int32_t)cal->par_p3 << 5)) >> 3) +
             (((int32_t)cal->par_p2 * p_var1) >> 1);
    p_var1 = p_var1 >> 18;
    p_var1 = ((32768 + p_var1) * (int32_t)cal->par_p1) >> 15;

    if (p_var1 != 0) {
        int32_t press_comp = 1048576 - raw_press;
        press_comp = (int32_t)((press_comp - (p_var2 >> 12)) * (uint32_t)3125);
        if (press_comp >= 0x40000000) {
            press_comp = (press_comp / p_var1) << 1;
        } else {
            press_comp = (press_comp << 1) / p_var1;
        }
        int32_t v1 = ((int32_t)cal->par_p9 * (int32_t)(((press_comp >> 3) * (press_comp >> 3)) >> 13)) >> 12;
        int32_t v2 = ((int32_t)(press_comp >> 2) * (int32_t)cal->par_p8) >> 13;
        int32_t v3 = ((int32_t)(press_comp >> 8) * (int32_t)(press_comp >> 8) * (int32_t)(press_comp >> 8) *
                      (int32_t)cal->par_p10) >> 17;
        press_comp = press_comp + ((v1 + v2 + v3 + ((int32_t)cal->par_p7 << 7)) >> 4);
        out->pressure_pa = press_comp;
    }

    /* --- Humidity (0.01 %RH) --- */
    int32_t temp_scaled = out->temperature_c100;
    int32_t h_var1 = raw_hum - ((int32_t)cal->par_h1 << 4) -
                     (((temp_scaled * (int32_t)cal->par_h3) / 100) >> 1);
    int32_t h_var2 = ((int32_t)cal->par_h2 *
                      (((temp_scaled * (int32_t)cal->par_h4) / 100) +
                       (((temp_scaled * ((temp_scaled * (int32_t)cal->par_h5) / 100)) >> 6) / 100) +
                       (1 << 14))) >> 10;
    int32_t h_var3 = h_var1 * h_var2;
    int32_t h_var4 = (((int32_t)cal->par_h6 << 7) +
                      ((temp_scaled * (int32_t)cal->par_h7) / 100)) >> 4;
    int32_t h_var5 = ((h_var3 >> 14) * (h_var3 >> 14)) >> 10;
    int32_t h_var6 = (h_var4 * h_var5) >> 1;
    int32_t calc_hum = (((h_var3 + h_var6) >> 10) * 1000) >> 12;
    if (calc_hum < 0) calc_hum = 0;
    if (calc_hum > 100000) calc_hum = 100000;
    out->humidity_cpercent = (calc_hum + 5) / 10;

    /* --- Gas Resistance (Ohms) --- */
    if (gas_range < 16u) {
        int64_t g_var1 = ((1340LL + (5LL * (int64_t)cal->range_sw_err)) *
                          (int64_t)LOOKUP_TABLE1[gas_range]) >> 16;
        int64_t g_var2 = (((int64_t)raw_gas << 15) - 16777216LL) + g_var1;
        if (g_var2 != 0) {
            int64_t g_var3 = ((int64_t)LOOKUP_TABLE2[gas_range] * g_var1) >> 9;
            out->gas_res_ohm = (int32_t)((g_var3 + (g_var2 >> 1)) / g_var2);
        }
    }
}

bool bme680_read(bme680_reading_t *out) {
    if (!out || !g.detected) return false;

    /* Re-arm heater target based on last measured ambient temperature if available */
    int16_t amb = g.have_last ? (int16_t)(g.last.temperature_c100 / 100) : 25;
    uint8_t rh = bme680_calc_res_heat(320u, amb, &g.cal);
    (void)wr(REG_RES_HEAT_0, rh);

    /* Trigger forced mode */
    if (!wr(REG_CTRL_MEAS, CTRL_MEAS_FORCED)) return fail("ctrl_meas write");

    /* Initial wait for conversion & heating: ~150 ms heating + ~10 ms TPH */
    uint64_t heat_until = time_get_ms() + 160u;
    while (time_get_ms() < heat_until) sched_yield();

    /* Poll for completion (up to 200 ms timeout) */
    uint64_t deadline = time_get_ms() + 200u;
    for (;;) {
        uint8_t status = 0;
        if (!rd(REG_MEAS_STAT_0, &status, 1u)) return fail("status poll");
        if (!(status & (MEAS_STAT_MEASURING | MEAS_STAT_GAS_MEAS))) break;
        if (time_get_ms() >= deadline) {
            return fail("measurement timeout");
        }
        sched_yield();
    }

    /* Read TPH data registers (0x1F..0x26: 8 bytes) */
    uint8_t d[8];
    if (!rd(REG_PRESS_MSB, d, 8u)) return fail("tph data read");

    /* Read Gas data registers (0x2A..0x2B: 2 bytes) */
    uint8_t gd[2];
    if (!rd(REG_GAS_R_MSB, gd, 2u)) return fail("gas data read");

    int32_t raw_press = (int32_t)(((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | (d[2] >> 4));
    int32_t raw_temp  = (int32_t)(((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | (d[5] >> 4));
    int32_t raw_hum   = (int32_t)(((uint32_t)d[6] << 8) | d[7]);

    int32_t raw_gas   = (int32_t)(((uint16_t)gd[0] << 2) | ((uint16_t)gd[1] >> 6));
    uint8_t gas_range = gd[1] & 0x0Fu;
    bool gas_valid    = (gd[1] & 0x20u) != 0;
    bool heat_stab    = (gd[1] & 0x10u) != 0;

    bme680_compensate(&g.cal, raw_temp, raw_press, raw_hum, raw_gas, gas_range, out);
    out->gas_valid = gas_valid;
    out->heat_stab = heat_stab;

    g.last = *out;
    g.last_ms = time_get_ms();
    g.have_last = true;
    g.read_count++;
    return true;
}

uint32_t bme680_selftest(bool report) {
    static const bme680_calib_t ref_cal = {
        .par_t1 = 26182, .par_t2 = 26344, .par_t3 = 3,
        .par_p1 = 36171, .par_p2 = -10502, .par_p3 = 30, .par_p4 = 6554,
        .par_p5 = 11, .par_p6 = 30, .par_p7 = 45, .par_p8 = -1141,
        .par_p9 = -812, .par_p10 = 30,
        .par_h1 = 735, .par_h2 = 1018, .par_h3 = 0, .par_h4 = 45,
        .par_h5 = 20, .par_h6 = 120, .par_h7 = -100,
        .par_gh1 = -16, .par_gh2 = -10321, .par_gh3 = 18,
        .res_heat_val = 50, .res_heat_range = 1, .range_sw_err = 0,
    };

    const int32_t want_t = 2547;
    const int32_t want_p = 103475;
    const int32_t want_h = 4195;
    const int32_t want_g = 8072289;
    const uint8_t want_rh = 117;
    const uint8_t want_gw = 101;

    bme680_reading_t r;
    bme680_compensate(&ref_cal, 500000, 350000, 20000, 500, 0, &r);

    uint32_t failed = 0;
    if (r.temperature_c100 != want_t) {
        failed++;
        if (report) cprintf("  bme680 temperature: got %ld, want %ld\n",
                            (long)r.temperature_c100, (long)want_t);
    }
    if (r.pressure_pa != want_p) {
        failed++;
        if (report) cprintf("  bme680 pressure: got %ld, want %ld\n",
                            (long)r.pressure_pa, (long)want_p);
    }
    if (r.humidity_cpercent != want_h) {
        failed++;
        if (report) cprintf("  bme680 humidity: got %ld, want %ld\n",
                            (long)r.humidity_cpercent, (long)want_h);
    }
    if (r.gas_res_ohm != want_g) {
        failed++;
        if (report) cprintf("  bme680 gas_res: got %ld, want %ld\n",
                            (long)r.gas_res_ohm, (long)want_g);
    }

    uint8_t rh = bme680_calc_res_heat(320u, 25, &ref_cal);
    if (rh != want_rh) {
        failed++;
        if (report) cprintf("  bme680 res_heat: got %u, want %u\n", rh, want_rh);
    }

    uint8_t gw = bme680_calc_gas_wait(150u);
    if (gw != want_gw) {
        failed++;
        if (report) cprintf("  bme680 gas_wait: got %u, want %u\n", gw, want_gw);
    }

    if (report) cprintf("bme680 selftest: %lu case%s failed\n",
                        (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}

void bme680_print_status(void) {
    if (!g.detected) {
        cprintf("sensor: bme680 not detected\n");
        return;
    }
    bme680_reading_t r;
    if (!bme680_read(&r)) {
        cprintf("sensor: bme680 at 0x%02x read failed: %s (fail count %lu)\n",
                g.addr, g.last_fail ? g.last_fail : "?", (unsigned long)g.fail_count);
        return;
    }
    int32_t t = r.temperature_c100;
    int32_t t_w = t / 100;
    int32_t t_f = t < 0 ? -(t % 100) : (t % 100);
    int32_t p_hpa = r.pressure_pa / 100;
    int32_t p_frac = r.pressure_pa % 100;
    int32_t h_w = r.humidity_cpercent / 100;
    int32_t h_f = r.humidity_cpercent % 100;

    cprintf("bme680 at 0x%02x: %ld.%02ld C, %ld.%02ld hPa, %ld.%02ld %%RH, %ld Ohm%s\n",
            g.addr, (long)t_w, (long)t_f, (long)p_hpa, (long)p_frac,
            (long)h_w, (long)h_f, (long)r.gas_res_ohm,
            r.heat_stab ? " (heater stable)" : "");
}

/* --- Sensor Device Contract (Category D) --- */

static bool bme680_dev_init(struct sensor_dev *dev) {
    if (!bme680_init()) return false;
    dev->addr = g.addr;
    dev->chan_mask = (1u << SENSOR_CHAN_TEMP) |
                     (1u << SENSOR_CHAN_PRESSURE) |
                     (1u << SENSOR_CHAN_HUMIDITY) |
                     (1u << SENSOR_CHAN_GAS_RES);
    return true;
}

static bool bme680_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    bme680_reading_t r;
    return bme680_read(&r);
}

static bool bme680_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val) {
    (void)dev;
    if (!g.have_last || !out_val) return false;
    switch (chan) {
        case SENSOR_CHAN_TEMP:
            *out_val = g.last.temperature_c100;
            return true;
        case SENSOR_CHAN_PRESSURE:
            *out_val = g.last.pressure_pa;
            return true;
        case SENSOR_CHAN_HUMIDITY:
            *out_val = g.last.humidity_cpercent;
            return true;
        case SENSOR_CHAN_GAS_RES:
            *out_val = g.last.gas_res_ohm;
            return true;
        default:
            return false;
    }
}

static uint32_t bme680_dev_selftest(bool report) {
    return bme680_selftest(report);
}

static const sensor_ops_t bme680_ops = {
    .init      = bme680_dev_init,
    .sample    = bme680_dev_sample,
    .get_value = bme680_dev_get_value,
    .selftest  = bme680_dev_selftest,
};

sensor_dev_t bme680_sensor_dev = {
    .name      = "bme680",
    .addr      = 0,
    .chan_mask = (1u << SENSOR_CHAN_TEMP) |
                 (1u << SENSOR_CHAN_PRESSURE) |
                 (1u << SENSOR_CHAN_HUMIDITY) |
                 (1u << SENSOR_CHAN_GAS_RES),
    .ops       = &bme680_ops,
    .priv      = NULL,
};
