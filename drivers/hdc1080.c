#include "drivers/hdc1080.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <string.h>

/* Texas Instruments HDC1080 High Accuracy Humidity & Temperature Sensor.
 * Phase 46, plan/phase46_sensor_framework.md §4.
 *
 * Implements pure fixed-point arithmetic without FPU instructions.
 */

static struct {
    bool              detected;
    uint8_t           addr;
    hdc1080_reading_t last;
    uint64_t          last_ms;
    bool              have_last;
    uint32_t          read_count;
    uint32_t          fail_count;
    const char       *last_failure;
} g;

static bool fail(const char *reason) {
    g.fail_count++;
    g.last_failure = reason;
    return false;
}

bool hdc1080_is_detected(void) {
    return g.detected;
}

uint8_t hdc1080_address(void) {
    return g.addr;
}

const char *hdc1080_part_name(void) {
    return "hdc1080";
}

bool hdc1080_cached(hdc1080_reading_t *out, uint32_t *age_s) {
    if (!g.have_last || !out) return false;
    *out = g.last;
    if (age_s) {
        uint64_t now = time_get_ms();
        *age_s = (now >= g.last_ms) ? (uint32_t)((now - g.last_ms) / 1000u) : 0u;
    }
    return true;
}

uint32_t hdc1080_read_count(void) { return g.read_count; }
uint32_t hdc1080_fail_count(void) { return g.fail_count; }
const char *hdc1080_last_failure(void) { return g.last_failure; }

/* Convert raw 16-bit temperature to centi-degrees Celsius (0.01 C).
 * Formula: T = (raw / 65536) * 165 - 40 C
 * Fixed-point: T_c100 = (raw * 16500) / 65536 - 4000
 */
static int32_t hdc1080_calc_temp_c100(uint16_t raw_temp) {
    int32_t t = (int32_t)(((int64_t)raw_temp * 16500LL) >> 16);
    return t - 4000;
}

/* Convert raw 16-bit humidity to centi-%RH (0.01 %RH).
 * Formula: RH = (raw / 65536) * 100 %RH
 * Fixed-point: RH_c100 = (raw * 10000) / 65536
 */
static int32_t hdc1080_calc_hum_c100(uint16_t raw_hum) {
    return (int32_t)(((int64_t)raw_hum * 10000LL) >> 16);
}

/* Convert raw 16-bit humidity to milli-%RH (0.001 %RH).
 * Fixed-point: RH_rh1000 = (raw * 100000) / 65536
 */
static int32_t hdc1080_calc_hum_rh1000(uint16_t raw_hum) {
    return (int32_t)(((int64_t)raw_hum * 100000LL) >> 16);
}

bool hdc1080_init(void) {
    g.detected = false;
    g.addr = HDC1080_I2C_ADDR;

    uint16_t manuf_id = 0;
    if (!i2c_reg_read_u16_be(g.addr, HDC1080_REG_MANUF_ID, &manuf_id)) {
        return false;
    }
    if (manuf_id != HDC1080_MANUFACTURER_ID) {
        return false;
    }

    uint16_t dev_id = 0;
    if (!i2c_reg_read_u16_be(g.addr, HDC1080_REG_DEV_ID, &dev_id)) {
        return false;
    }
    if (dev_id != HDC1080_DEVICE_ID) {
        return false;
    }

    /* Configure device for sequential Temp + Humidity measurement, 14-bit resolution */
    uint16_t config = HDC1080_CONFIG_MODE_BOTH | HDC1080_CONFIG_TRES_14 | HDC1080_CONFIG_HRES_14;
    if (!i2c_reg_write_u16_be(g.addr, HDC1080_REG_CONFIG, config)) {
        return false;
    }

    g.detected = true;
    printk("[HDC1080] HDC1080 temperature & humidity sensor detected at 0x%02x (TI ID: 0x%04x).\n",
           g.addr, manuf_id);
    return true;
}

bool hdc1080_read(hdc1080_reading_t *out) {
    if (!out || !g.detected) return false;

    /* 1. Trigger measurement by writing pointer 0x00 (Temperature register) */
    uint8_t ptr = HDC1080_REG_TEMP;
    if (!i2c_xfer(g.addr, &ptr, 1, NULL, 0)) {
        return fail("trigger conversion");
    }

    /* 2. Wait for 14-bit Temp + 14-bit Hum conversion (~13 ms max) */
    uint64_t wait_until = time_get_ms() + 15u;
    while (time_get_ms() < wait_until) sched_yield();

    /* 3. Read 4 bytes: 2 bytes Temp, 2 bytes Humidity */
    uint8_t buf[4];
    if (!i2c_xfer(g.addr, NULL, 0, buf, 4)) {
        return fail("read measurement data");
    }

    uint16_t raw_temp = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    uint16_t raw_hum  = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);

    out->raw_temp        = raw_temp;
    out->raw_hum         = raw_hum;
    out->temp_c100       = hdc1080_calc_temp_c100(raw_temp);
    out->humidity_c100   = hdc1080_calc_hum_c100(raw_hum);
    out->humidity_rh1000 = hdc1080_calc_hum_rh1000(raw_hum);

    g.last = *out;
    g.last_ms = time_get_ms();
    g.have_last = true;
    g.read_count++;
    return true;
}

uint32_t hdc1080_selftest(bool report) {
    static const struct {
        uint16_t raw_t;
        uint16_t raw_h;
        int32_t  expected_t_c100;
        int32_t  expected_h_c100;
    } cases[] = {
        { 0x0000u, 0x0000u, -4000, 0 },
        { 0x6554u, 0x8000u,  2530, 5000 },
        { 0xFFFFu, 0xFFFFu, 12499, 9999 },
        { 0x3E80u, 0x4000u,     0, 2500 }, /* 0x3E80 = 16000: 16000*16500/65536 = 4028 - 4000 = 28 -> ~0 C */
    };

    uint32_t failed = 0;
    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int32_t t = hdc1080_calc_temp_c100(cases[i].raw_t);
        int32_t h = hdc1080_calc_hum_c100(cases[i].raw_h);

        /* Allow +/- 50 centi-units (0.5 C / 0.5 %RH) due to integer truncation */
        int32_t dt = t - cases[i].expected_t_c100;
        int32_t dh = h - cases[i].expected_h_c100;
        if (dt < 0) dt = -dt;
        if (dh < 0) dh = -dh;

        if (dt > 50 || dh > 50) {
            failed++;
            if (report) {
                cprintf("  [FAIL] HDC1080 case %lu: T=%ld (exp %ld), H=%ld (exp %ld)\n",
                        (unsigned long)i, (long)t, (long)cases[i].expected_t_c100,
                        (long)h, (long)cases[i].expected_h_c100);
            }
        }
    }

    if (report) {
        cprintf("hdc1080 selftest: %lu cases failed\n", (unsigned long)failed);
    }
    return failed;
}

/* --- Environmental Sensor Device-Class Integration (Category D, Phase 46) --- */

static bool hdc1080_dev_init(sensor_dev_t *dev) {
    (void)dev;
    return hdc1080_init();
}

static bool hdc1080_dev_sample(sensor_dev_t *dev) {
    (void)dev;
    hdc1080_reading_t r;
    return hdc1080_read(&r);
}

static bool hdc1080_dev_get_value(sensor_dev_t *dev, sensor_chan_t chan, int32_t *out_val) {
    (void)dev;
    if (!out_val || !g.have_last) return false;
    switch (chan) {
    case SENSOR_CHAN_TEMP:
        *out_val = g.last.temp_c100;
        return true;
    case SENSOR_CHAN_HUMIDITY:
        *out_val = g.last.humidity_c100;
        return true;
    default:
        return false;
    }
}

static const sensor_ops_t s_hdc1080_ops = {
    .init      = hdc1080_dev_init,
    .sample    = hdc1080_dev_sample,
    .get_value = hdc1080_dev_get_value,
    .selftest  = hdc1080_selftest,
};

sensor_dev_t hdc1080_sensor_dev = {
    .name      = "hdc1080",
    .addr      = HDC1080_I2C_ADDR,
    .chan_mask = (1u << SENSOR_CHAN_TEMP) | (1u << SENSOR_CHAN_HUMIDITY),
    .ops       = &s_hdc1080_ops,
    .priv      = NULL,
};
