#include "drivers/tmp117.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "kernel/sched.h"
#include <string.h>

static struct {
    bool             detected;
    uint8_t          addr;
    uint8_t          bus;
    tmp117_reading_t last;
    uint64_t         last_ms;
    bool             have_last;
    uint32_t         read_count;
    uint32_t         fail_count;
    const char      *last_fail;
} g;

static bool fail(const char *where) {
    g.last_fail = where;
    g.fail_count++;
    return false;
}

bool tmp117_is_detected(void) { return g.detected; }
uint8_t tmp117_address(void)  { return g.addr; }
uint8_t tmp117_bus(void)      { return g.bus; }

int32_t tmp117_calc_temp_c100(int16_t raw_val) {
    /* 1 LSB is 0.0078125 °C (1/128 °C). Centi-degrees = (raw * 100) / 128 = (raw * 25) / 32 */
    int32_t v = (int32_t)raw_val;
    if (v >= 0) {
        return (v * 25 + 16) / 32;
    } else {
        return (v * 25 - 16) / 32;
    }
}

bool tmp117_init(void) {
    static const uint8_t ADDRS[4] = { 0x48u, 0x49u, 0x4Au, 0x4Bu };
    uint8_t buses = i2c_bus_count();

    for (uint8_t b = 0; b < buses; b++) {
        for (uint32_t i = 0; i < 4u; i++) {
            uint8_t addr = ADDRS[i];
            uint16_t dev_id = 0;
            if (!i2c_reg_read_u16_be_bus(b, addr, TMP117_REG_DEVICE_ID, &dev_id)) {
                continue;
            }
            if ((dev_id & 0x0FFFu) != TMP117_DEVICE_ID_VAL) continue;

            /* Set configuration: continuous conversion, 8 averages, 125 ms conversion cycle */
            (void)i2c_reg_write_u16_be_bus(b, addr, TMP117_REG_CONFIG, TMP117_CONFIG_DEFAULT);

            g.detected = true;
            g.addr = addr;
            g.bus = b;

            printk("[TMP117] TMP117 ±0.1 C NIST-traceable temperature sensor at bus %u 0x%02x (rev 0x%02x).\n",
                   b, addr, (unsigned)(dev_id >> 12));
            return true;
        }
    }

    g.detected = false;
    g.addr = 0;
    return false;
}

bool tmp117_read(tmp117_reading_t *out) {
    if (!out || !g.detected) return false;

    uint16_t raw16 = 0;
    if (!i2c_reg_read_u16_be_bus(g.bus, g.addr, TMP117_REG_TEMP, &raw16)) {
        return fail("read temperature");
    }

    /* 0x8000 (-256 °C) is the power-up reset indicator indicating first conversion pending */
    if (raw16 == 0x8000u) {
        return fail("conversion pending");
    }

    uint16_t cfg = 0;
    (void)i2c_reg_read_u16_be_bus(g.bus, g.addr, TMP117_REG_CONFIG, &cfg);

    int16_t s_raw = (int16_t)raw16;
    out->raw_temp   = s_raw;
    out->data_ready = (cfg & (1u << 13)) != 0;
    out->high_alert = (cfg & (1u << 15)) != 0;
    out->low_alert  = (cfg & (1u << 14)) != 0;
    out->temp_c100  = tmp117_calc_temp_c100(s_raw);

    g.last = *out;
    g.last_ms = time_get_ms();
    g.have_last = true;
    g.read_count++;
    return true;
}

uint32_t tmp117_selftest(bool report) {
    static const struct {
        int16_t raw;
        int32_t expected_c100;
        const char *name;
    } cases[] = {
        { 0x0000,      0, "0.00 C" },
        { 0x0C80,   2500, "+25.00 C" },
        { (int16_t)0xF380, -2500, "-25.00 C" },
        { 0x0080,    100, "+1.00 C" },
        { 0x0001,      1, "+0.0078125 C -> 1 cC" },
        { -1,         -1, "-0.0078125 C -> -1 cC" },
        { 0x3200,  10000, "+100.00 C" },
    };

    uint32_t failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int32_t got = tmp117_calc_temp_c100(cases[i].raw);
        if (got != cases[i].expected_c100) {
            failed++;
            if (report) {
                cprintf("FAIL tmp117 %s: raw=0x%04x expected=%ld got=%ld\n",
                        cases[i].name, (uint16_t)cases[i].raw, (long)cases[i].expected_c100, (long)got);
            }
        }
    }

    if (report) {
        cprintf("tmp117 selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1 ? "" : "s");
    }
    return failed;
}

/* --- Sensor Device-Class Contract --- */

static bool tmp117_dev_init(struct sensor_dev *dev) {
    if (!tmp117_init()) return false;
    dev->addr = g.addr;
    dev->bus = g.bus;
    dev->chan_mask = (1u << SENSOR_CHAN_TEMP);
    return true;
}

static bool tmp117_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    tmp117_reading_t r;
    return tmp117_read(&r);
}

static bool tmp117_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val) {
    (void)dev;
    if (!g.have_last || !out_val) return false;
    if (chan == SENSOR_CHAN_TEMP) {
        *out_val = g.last.temp_c100;
        return true;
    }
    return false;
}

static uint32_t tmp117_dev_selftest(bool report) {
    return tmp117_selftest(report);
}

static const sensor_ops_t tmp117_ops = {
    .init      = tmp117_dev_init,
    .sample    = tmp117_dev_sample,
    .get_value = tmp117_dev_get_value,
    .selftest  = tmp117_dev_selftest,
};

sensor_dev_t tmp117_sensor_dev = {
    .name      = "tmp117",
    .addr      = 0,
    .bus       = 0,
    .chan_mask = (1u << SENSOR_CHAN_TEMP),
    .ops       = &tmp117_ops,
    .priv      = NULL,
};
