#include "drivers/mics6814.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <string.h>

/* Grove Multichannel Gas Sensor Driver (SGX Sensortech MiCS-6814).
 * Phase 46, plan/phase46_sensor_framework.md.
 *
 * Implements:
 * - Dual-firmware auto-detection (V1 4-byte checksum protocol, V2 2-byte register protocol)
 * - Measurement extraction: CO, NO2, NH3
 * - Pure integer fixed-point Taylor expansion (zero floating-point)
 */

#define CMD_V1_R_NH3             0x01u
#define CMD_V1_R_CO              0x02u
#define CMD_V1_R_NO2             0x03u
#define CMD_V1_R0_NH3            0x11u
#define CMD_V1_R0_CO             0x12u
#define CMD_V1_R0_NO2            0x13u
#define CMD_V1_POWER_OFF         0x20u
#define CMD_V1_POWER_ON          0x21u

#define CMD_V2_CH_NH3            0x01u
#define CMD_V2_CH_CO             0x02u
#define CMD_V2_CH_NO2            0x03u
#define CMD_V2_READ_EEPROM       0x06u
#define CMD_V2_CONTROL_PWR       0x0Bu

#define ADDR_EEPROM_IS_SET       0x00u
#define ADDR_EEPROM_FACTORY_NH3  0x02u
#define ADDR_EEPROM_FACTORY_CO   0x04u
#define ADDR_EEPROM_FACTORY_NO2  0x06u
#define ADDR_EEPROM_USER_NH3     0x08u
#define ADDR_EEPROM_USER_CO      0x0Au
#define ADDR_EEPROM_USER_NO2     0x0Cu

static struct {
    bool                detected;
    uint8_t             addr;
    uint8_t             fw_version; /* 1 or 2 */

    mics6814_reading_t  last;
    uint64_t            last_ms;
    uint64_t            heater_on_ms;
    bool                have_last;

    uint32_t            read_count;
    uint32_t            fail_count;
    const char         *last_fail;
} g;

static bool fail(const char *where) {
    g.last_fail = where;
    g.fail_count++;
    return false;
}

bool        mics6814_is_detected(void)  { return g.detected; }
uint8_t     mics6814_address(void)      { return g.addr; }
const char *mics6814_part_name(void)    { return "mics6814"; }

bool mics6814_is_warming_up(uint32_t *remaining_s) {
    if (!g.detected || g.heater_on_ms == 0) return false;
    uint64_t now = time_get_ms();
    uint64_t total_ms = (uint64_t)MICS6814_WARMUP_PERIOD_S * 1000u;
    if (now >= g.heater_on_ms && (now - g.heater_on_ms) < total_ms) {
        if (remaining_s) *remaining_s = (uint32_t)((total_ms - (now - g.heater_on_ms)) / 1000u);
        return true;
    }
    if (remaining_s) *remaining_s = 0;
    return false;
}

uint32_t    mics6814_read_count(void)   { return g.read_count; }
uint32_t    mics6814_fail_count(void)   { return g.fail_count; }
const char *mics6814_last_failure(void) { return g.last_fail; }

bool mics6814_cached(mics6814_reading_t *out, uint32_t *age_s) {
    if (!out || !g.have_last) return false;
    *out = g.last;
    if (age_s) {
        uint64_t now = time_get_ms();
        *age_s = (now >= g.last_ms) ? (uint32_t)((now - g.last_ms) / 1000u) : 0u;
    }
    return true;
}

/* --- Fixed-Point Math Engine (Scale 10^6, Pure Integer) --- */

static int32_t fixed_ln(int32_t val_million) {
    if (val_million <= 0) return -14000000;
    int32_t val = val_million;
    int32_t k = 0;
    while (val >= 2000000) { val >>= 1; k++; }
    while (val < 1000000)  { val <<= 1; k--; }
    int64_t num = val - 1000000;
    int64_t den = val + 1000000;
    int64_t z = (num * 1000000) / den;
    int64_t z2 = (z * z) / 1000000;
    int64_t t1 = z;
    int64_t t3 = (t1 * z2) / 1000000;
    int64_t t5 = (t3 * z2) / 1000000;
    int64_t t7 = (t5 * z2) / 1000000;
    int64_t t9 = (t7 * z2) / 1000000;
    int64_t ln_m = 2 * (t1 + t3 / 3 + t5 / 5 + t7 / 7 + t9 / 9);
    return (int32_t)(k * 693147 + ln_m);
}

static int32_t fixed_exp(int32_t u_million) {
    if (u_million < -14000000) return 0;
    if (u_million > 14000000) u_million = 14000000;
    int32_t ln2 = 693147;
    int32_t k = u_million / ln2;
    int32_t r = u_million % ln2;
    if (r < 0) { r += ln2; k--; }
    int64_t t0 = 1000000;
    int64_t t1 = r;
    int64_t t2 = (t1 * r) / 2000000;
    int64_t t3 = (t2 * r) / 3000000;
    int64_t t4 = (t3 * r) / 4000000;
    int64_t t5 = (t4 * r) / 5000000;
    int64_t t6 = (t5 * r) / 6000000;
    int64_t t7 = (t6 * r) / 7000000;
    int64_t e_r = t0 + t1 + t2 + t3 + t4 + t5 + t6 + t7;
    int64_t val = (k >= 0) ? (e_r << k) : (e_r >> (-k));
    return (int32_t)val;
}

/* Computes gas concentration in 0.01 ppm (centi-ppm) from ratio Q10 (1024 = 1.0) */
static int32_t mics6814_calc_gas(uint16_t ratio_q10, int kind) {
    if (ratio_q10 == 0) return 0;
    int64_t r_million = ((int64_t)ratio_q10 * 1000000) / 1024;
    int32_t ln_r = fixed_ln((int32_t)r_million);

    int64_t u = 0;
    int64_t A = 0;

    if (kind == 0) {
        /* CO: c = 4.385 * ratio^(-1.179) */
        u = ((int64_t)ln_r * -1179) / 1000;
        A = 438500; /* milli-centi-ppm */
    } else if (kind == 1) {
        /* NO2: c = (1/6.855) * ratio^(1.007) */
        u = ((int64_t)ln_r * 1007) / 1000;
        A = 14588;
    } else if (kind == 2) {
        /* NH3: c = (1/1.47) * ratio^(-1.670) */
        u = ((int64_t)ln_r * -1670) / 1000;
        A = 68027;
    } else {
        return 0;
    }

    int32_t e_u = fixed_exp((int32_t)u);
    int64_t res = (A * e_u + 500000000) / 1000000000;
    if (res > 0x7FFFFFFF) res = 0x7FFFFFFF;
    return (int32_t)res;
}

/* --- Hardware Communication Helpers --- */

static bool mics6814_v1_read_cmd(uint8_t cmd, uint16_t *out_val) {
    uint8_t c = cmd;
    if (!i2c_xfer(g.addr, &c, 1u, NULL, 0u)) return false;

    uint64_t wait = time_get_ms() + 5u;
    while (time_get_ms() < wait) sched_yield();

    uint8_t buf[4];
    if (!i2c_xfer(g.addr, NULL, 0u, buf, 4u)) return false;

    uint8_t sum = (uint8_t)(buf[0] + buf[1] + buf[2]);
    if (sum != buf[3]) return false;

    if (out_val) *out_val = (uint16_t)(((uint16_t)buf[1] << 8) | buf[2]);
    return true;
}

static bool mics6814_v2_read_eeprom(uint8_t reg, uint16_t *out_val) {
    uint8_t cmd[2] = { CMD_V2_READ_EEPROM, reg };
    if (!i2c_xfer(g.addr, cmd, 2u, NULL, 0u)) return false;

    uint64_t wait = time_get_ms() + 5u;
    while (time_get_ms() < wait) sched_yield();

    uint8_t buf[2];
    if (!i2c_xfer(g.addr, NULL, 0u, buf, 2u)) return false;

    if (out_val) *out_val = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    return true;
}

static bool mics6814_v2_read_adc(uint8_t ch, uint16_t *out_val) {
    uint8_t c = ch;
    if (!i2c_xfer(g.addr, &c, 1u, NULL, 0u)) return false;

    uint64_t wait = time_get_ms() + 5u;
    while (time_get_ms() < wait) sched_yield();

    uint8_t buf[2];
    if (!i2c_xfer(g.addr, NULL, 0u, buf, 2u)) return false;

    if (out_val) *out_val = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    return true;
}

/* --- Driver Lifecycle --- */

bool mics6814_init(void) {
    g.detected = false;
    g.addr = MICS6814_DEFAULT_ADDR;
    g.fw_version = 1;

    /* 1. Probe address 0x04 */
    uint8_t dummy = 0;
    if (!i2c_xfer(g.addr, &dummy, 1u, NULL, 0u)) {
        return false;
    }

    uint64_t wait = time_get_ms() + 5u;
    while (time_get_ms() < wait) sched_yield();

    /* 2. Check firmware version */
    uint16_t is_set = 0;
    if (mics6814_v2_read_eeprom(ADDR_EEPROM_IS_SET, &is_set) && is_set == 1126u) {
        g.fw_version = 2;
    } else {
        g.fw_version = 1;
    }

    /* 3. Turn on sensor heater */
    g.heater_on_ms = time_get_ms();
    if (g.fw_version == 1) {
        uint8_t pwr_on = CMD_V1_POWER_ON;
        i2c_xfer(g.addr, &pwr_on, 1u, NULL, 0u);
    } else {
        uint8_t pwr_on[2] = { CMD_V2_CONTROL_PWR, 1u };
        i2c_xfer(g.addr, pwr_on, 2u, NULL, 0u);
    }

    wait = time_get_ms() + 10u;
    while (time_get_ms() < wait) sched_yield();

    /* 4. Load baseline R0 values */
    memset(&g.last, 0, sizeof(g.last));
    g.last.fw_version = g.fw_version;

    if (g.fw_version == 1) {
        mics6814_v1_read_cmd(CMD_V1_R0_NH3, &g.last.r0_nh3);
        mics6814_v1_read_cmd(CMD_V1_R0_CO,  &g.last.r0_co);
        mics6814_v1_read_cmd(CMD_V1_R0_NO2, &g.last.r0_no2);

        /* Default fallback baselines if uncalibrated (R0 = 0) */
        if (g.last.r0_nh3 == 0) g.last.r0_nh3 = 100u;
        if (g.last.r0_co  == 0) g.last.r0_co  = 100u;
        if (g.last.r0_no2 == 0) g.last.r0_no2 = 100u;
    } else {
        mics6814_v2_read_eeprom(ADDR_EEPROM_USER_NH3, &g.last.r0_nh3);
        mics6814_v2_read_eeprom(ADDR_EEPROM_USER_CO,  &g.last.r0_co);
        mics6814_v2_read_eeprom(ADDR_EEPROM_USER_NO2, &g.last.r0_no2);

        /* Fallback to factory baselines if user ADC is out of range */
        if (g.last.r0_nh3 == 0 || g.last.r0_nh3 >= 1023u)
            mics6814_v2_read_eeprom(ADDR_EEPROM_FACTORY_NH3, &g.last.r0_nh3);
        if (g.last.r0_co == 0 || g.last.r0_co >= 1023u)
            mics6814_v2_read_eeprom(ADDR_EEPROM_FACTORY_CO,  &g.last.r0_co);
        if (g.last.r0_no2 == 0 || g.last.r0_no2 >= 1023u)
            mics6814_v2_read_eeprom(ADDR_EEPROM_FACTORY_NO2, &g.last.r0_no2);

        if (g.last.r0_nh3 == 0 || g.last.r0_nh3 >= 1023u) g.last.r0_nh3 = 512u;
        if (g.last.r0_co  == 0 || g.last.r0_co  >= 1023u) g.last.r0_co  = 512u;
        if (g.last.r0_no2 == 0 || g.last.r0_no2 >= 1023u) g.last.r0_no2 = 512u;
    }

    g.detected = true;
    printk("[MiCS-6814] Grove Multichannel Gas Sensor at 0x%02x (firmware v%u, R0: NH3=%u, CO=%u, NO2=%u).\n",
           g.addr, g.fw_version, g.last.r0_nh3, g.last.r0_co, g.last.r0_no2);
    return true;
}

bool mics6814_read(mics6814_reading_t *out) {
    if (!out || !g.detected) return false;

    mics6814_reading_t r = g.last;

    if (g.fw_version == 1) {
        if (!mics6814_v1_read_cmd(CMD_V1_R_NH3, &r.rs_nh3) ||
            !mics6814_v1_read_cmd(CMD_V1_R_CO,  &r.rs_co)  ||
            !mics6814_v1_read_cmd(CMD_V1_R_NO2, &r.rs_no2)) {
            return fail("v1 read failed");
        }

        uint16_t r0_nh3 = r.r0_nh3 ? r.r0_nh3 : 1u;
        uint16_t r0_co  = r.r0_co  ? r.r0_co  : 1u;
        uint16_t r0_no2 = r.r0_no2 ? r.r0_no2 : 1u;

        r.ratio_nh3_q10 = (uint16_t)(((uint32_t)r.rs_nh3 * 1024u) / r0_nh3);
        r.ratio_co_q10  = (uint16_t)(((uint32_t)r.rs_co  * 1024u) / r0_co);
        r.ratio_no2_q10 = (uint16_t)(((uint32_t)r.rs_no2 * 1024u) / r0_no2);
    } else {
        uint16_t an_nh3 = 0, an_co = 0, an_no2 = 0;
        if (!mics6814_v2_read_adc(CMD_V2_CH_NH3, &an_nh3) ||
            !mics6814_v2_read_adc(CMD_V2_CH_CO,  &an_co)  ||
            !mics6814_v2_read_adc(CMD_V2_CH_NO2, &an_no2)) {
            return fail("v2 read failed");
        }
        r.rs_nh3 = an_nh3;
        r.rs_co  = an_co;
        r.rs_no2 = an_no2;

        /* Ratio = (An / A0) * (1023 - A0) / (1023 - An) */
        uint32_t a0_nh3 = r.r0_nh3 ? r.r0_nh3 : 512u;
        uint32_t a0_co  = r.r0_co  ? r.r0_co  : 512u;
        uint32_t a0_no2 = r.r0_no2 ? r.r0_no2 : 512u;

        uint32_t den_nh3 = a0_nh3 * (1023u > an_nh3 ? 1023u - an_nh3 : 1u);
        uint32_t den_co  = a0_co  * (1023u > an_co  ? 1023u - an_co  : 1u);
        uint32_t den_no2 = a0_no2 * (1023u > an_no2 ? 1023u - an_no2 : 1u);

        r.ratio_nh3_q10 = (uint16_t)(((uint64_t)an_nh3 * (1023u - a0_nh3) * 1024u) / (den_nh3 ? den_nh3 : 1u));
        r.ratio_co_q10  = (uint16_t)(((uint64_t)an_co  * (1023u - a0_co)  * 1024u) / (den_co  ? den_co  : 1u));
        r.ratio_no2_q10 = (uint16_t)(((uint64_t)an_no2 * (1023u - a0_no2) * 1024u) / (den_no2 ? den_no2 : 1u));
    }

    r.co_c_ppm  = mics6814_calc_gas(r.ratio_co_q10,  0);
    r.no2_c_ppm = mics6814_calc_gas(r.ratio_no2_q10, 1);
    r.nh3_c_ppm = mics6814_calc_gas(r.ratio_nh3_q10, 2);

    uint32_t rem = 0;
    r.is_warming_up = mics6814_is_warming_up(&rem);
    r.warmup_remaining_s = (uint16_t)rem;

    *out = r;
    g.last = r;
    g.last_ms = time_get_ms();
    g.have_last = true;
    g.read_count++;
    return true;
}

bool mics6814_get_r0(uint16_t *r0_nh3, uint16_t *r0_co, uint16_t *r0_no2) {
    if (!g.detected) return false;
    if (r0_nh3) *r0_nh3 = g.last.r0_nh3;
    if (r0_co)  *r0_co  = g.last.r0_co;
    if (r0_no2) *r0_no2 = g.last.r0_no2;
    return true;
}

bool mics6814_set_r0(uint16_t r0_nh3, uint16_t r0_co, uint16_t r0_no2) {
    if (!g.detected) return false;
    if (r0_nh3) g.last.r0_nh3 = r0_nh3;
    if (r0_co)  g.last.r0_co  = r0_co;
    if (r0_no2) g.last.r0_no2 = r0_no2;
    return true;
}

uint32_t mics6814_selftest(bool report) {
    uint32_t failed = 0;

    static const struct {
        uint16_t ratio_q10;
        int32_t  want_co;
        int32_t  want_no2;
        int32_t  want_nh3;
    } vectors[] = {
        { 512u,  993,  7, 216 },
        { 1024u, 439, 15,  68 },
        { 1536u, 272, 22,  35 },
        { 2048u, 194, 29,  21 },
    };

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        int32_t co  = mics6814_calc_gas(vectors[i].ratio_q10, 0);
        int32_t no2 = mics6814_calc_gas(vectors[i].ratio_q10, 1);
        int32_t nh3 = mics6814_calc_gas(vectors[i].ratio_q10, 2);

        if (co != vectors[i].want_co || no2 != vectors[i].want_no2 || nh3 != vectors[i].want_nh3) {
            failed++;
            if (report) {
                cprintf("  mics6814 vector %u (q10=%u): got CO=%ld NO2=%ld NH3=%ld, want CO=%ld NO2=%ld NH3=%ld\n",
                        (unsigned)i, vectors[i].ratio_q10,
                        (long)co, (long)no2, (long)nh3,
                        (long)vectors[i].want_co, (long)vectors[i].want_no2, (long)vectors[i].want_nh3);
            }
        }
    }

    if (report) {
        cprintf("mics6814 selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1u ? "" : "s");
    }
    return failed;
}

void mics6814_print_status(void) {
    if (!g.detected) {
        cprintf("sensor: mics6814 not detected\n");
        return;
    }
    mics6814_reading_t r;
    if (!mics6814_read(&r)) {
        cprintf("sensor: mics6814 at 0x%02x read failed: %s (fail count %lu)\n",
                g.addr, g.last_fail ? g.last_fail : "?", (unsigned long)g.fail_count);
        return;
    }
    uint32_t rem = 0;
    if (mics6814_is_warming_up(&rem)) {
        cprintf("mics6814 at 0x%02x (v%u, warming up: %lum %02lus left): %ld.%02ld ppm CO, %ld.%02ld ppm NO2, %ld.%02ld ppm NH3\n",
                g.addr, r.fw_version, (unsigned long)(rem / 60), (unsigned long)(rem % 60),
                (long)(r.co_c_ppm / 100), (long)(r.co_c_ppm % 100),
                (long)(r.no2_c_ppm / 100), (long)(r.no2_c_ppm % 100),
                (long)(r.nh3_c_ppm / 100), (long)(r.nh3_c_ppm % 100));
    } else {
        cprintf("mics6814 at 0x%02x (v%u): %ld.%02ld ppm CO, %ld.%02ld ppm NO2, %ld.%02ld ppm NH3\n",
                g.addr, r.fw_version,
                (long)(r.co_c_ppm / 100), (long)(r.co_c_ppm % 100),
                (long)(r.no2_c_ppm / 100), (long)(r.no2_c_ppm % 100),
                (long)(r.nh3_c_ppm / 100), (long)(r.nh3_c_ppm % 100));
    }
}

/* --- Sensor Device Contract (Category D) --- */

static bool mics6814_dev_init(struct sensor_dev *dev) {
    if (!mics6814_init()) return false;
    dev->addr = g.addr;
    dev->chan_mask = (1u << SENSOR_CHAN_CO) | (1u << SENSOR_CHAN_NO2) | (1u << SENSOR_CHAN_NH3);
    return true;
}

static bool mics6814_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    mics6814_reading_t r;
    return mics6814_read(&r);
}

static bool mics6814_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val) {
    (void)dev;
    if (!g.have_last || !out_val) return false;
    if (chan == SENSOR_CHAN_CO) {
        *out_val = g.last.co_c_ppm;
        return true;
    }
    if (chan == SENSOR_CHAN_NO2) {
        *out_val = g.last.no2_c_ppm;
        return true;
    }
    if (chan == SENSOR_CHAN_NH3) {
        *out_val = g.last.nh3_c_ppm;
        return true;
    }
    return false;
}

static uint32_t mics6814_dev_selftest(bool report) {
    return mics6814_selftest(report);
}

static const sensor_ops_t mics6814_ops = {
    .init      = mics6814_dev_init,
    .sample    = mics6814_dev_sample,
    .get_value = mics6814_dev_get_value,
    .selftest  = mics6814_dev_selftest,
};

sensor_dev_t mics6814_sensor_dev = {
    .name      = "mics6814",
    .addr      = 0,
    .chan_mask = (1u << SENSOR_CHAN_CO) | (1u << SENSOR_CHAN_NO2) | (1u << SENSOR_CHAN_NH3),
    .ops       = &mics6814_ops,
    .priv      = NULL,
};
