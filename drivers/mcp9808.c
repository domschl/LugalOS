#include "drivers/mcp9808.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "kernel/sched.h"
#include <string.h>

static struct {
    bool              detected;
    uint8_t           addr;
    uint8_t           bus;
    mcp9808_reading_t last;
    uint64_t          last_ms;
    bool              have_last;
    uint32_t          read_count;
    uint32_t          fail_count;
    const char       *last_fail;
} g;

static bool fail(const char *where) {
    g.last_fail = where;
    g.fail_count++;
    return false;
}

bool mcp9808_is_detected(void) { return g.detected; }
uint8_t mcp9808_address(void)  { return g.addr; }
uint8_t mcp9808_bus(void)      { return g.bus; }

int32_t mcp9808_calc_temp_c100(uint16_t raw_val) {
    uint8_t upper = (uint8_t)(raw_val >> 8) & 0x1Fu;
    uint8_t lower = (uint8_t)(raw_val & 0xFFu);
    uint16_t raw12 = (uint16_t)(((uint16_t)(upper & 0x0Fu) << 8) | lower);

    /* 1 LSB is 0.0625 °C (1/16 °C). Centi-degrees = (raw * 100) / 16 = (raw * 25) / 4 */
    if (upper & 0x10u) {
        /* TA < 0 °C: Temperature = 256 - (Upper x 16 + Lower / 16) */
        uint32_t diff = 4096u - (uint32_t)raw12;
        return - (int32_t)(((diff * 25u) + 2u) / 4u);
    } else {
        /* TA >= 0 °C: Temperature = (Upper x 16 + Lower / 16) */
        return (int32_t)((((uint32_t)raw12 * 25u) + 2u) / 4u);
    }
}

bool mcp9808_init(void) {
    uint8_t buses = i2c_bus_count();

    for (uint8_t b = 0; b < buses; b++) {
        for (uint8_t addr = 0x18u; addr <= 0x1Fu; addr++) {
            uint16_t manuf_id = 0;
            if (!i2c_reg_read_u16_be_bus(b, addr, MCP9808_REG_MANUF_ID, &manuf_id)) {
                continue;
            }
            if (manuf_id != MCP9808_MANUF_ID_VAL) continue;

            uint16_t dev_id = 0;
            if (!i2c_reg_read_u16_be_bus(b, addr, MCP9808_REG_DEV_ID, &dev_id)) {
                continue;
            }
            if ((dev_id >> 8) != (MCP9808_DEV_ID_VAL >> 8)) continue;

            /* Set maximum resolution: +0.0625 °C (250 ms conversion time) */
            (void)i2c_reg_write_u8_bus(b, addr, MCP9808_REG_RES, MCP9808_RES_0_0625);

            /* Clear shutdown / ensure continuous conversion */
            (void)i2c_reg_write_u16_be_bus(b, addr, MCP9808_REG_CONFIG, 0x0000u);

            g.detected = true;
            g.addr = addr;
            g.bus = b;

            printk("[MCP9808] MCP9808 ±0.25 C temperature sensor at bus %u 0x%02x (rev 0x%02x).\n",
                   b, addr, (unsigned)(dev_id & 0xFFu));
            return true;
        }
    }

    g.detected = false;
    g.addr = 0;
    return false;
}

bool mcp9808_read(mcp9808_reading_t *out) {
    if (!out || !g.detected) return false;

    uint16_t raw_val = 0;
    if (!i2c_reg_read_u16_be_bus(g.bus, g.addr, MCP9808_REG_T_AMBIENT, &raw_val)) {
        return fail("read ambient temp");
    }

    out->raw_temp    = raw_val;
    out->alert_crit  = (raw_val & 0x8000u) != 0;
    out->alert_upper = (raw_val & 0x4000u) != 0;
    out->alert_lower = (raw_val & 0x2000u) != 0;
    out->temp_c100   = mcp9808_calc_temp_c100(raw_val);

    g.last = *out;
    g.last_ms = time_get_ms();
    g.have_last = true;
    g.read_count++;
    return true;
}

uint32_t mcp9808_selftest(bool report) {
    static const struct {
        uint16_t raw;
        int32_t  expected_c100;
        const char *name;
    } cases[] = {
        { 0x0000u,     0, "0.00 C" },
        { 0x0191u,  2506, "+25.0625 C -> 2506 cC" },
        { 0x0080u,   800, "+8.00 C" },
        { 0x1E70u, -2500, "-25.00 C" },
        { 0x1FFFu,    -6, "-0.0625 C -> -6 cC" },
        { 0xE191u,  2506, "+25.0625 C with alert flags masked" },
    };

    uint32_t failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int32_t got = mcp9808_calc_temp_c100(cases[i].raw);
        if (got != cases[i].expected_c100) {
            failed++;
            if (report) {
                cprintf("FAIL mcp9808 %s: raw=0x%04x expected=%ld got=%ld\n",
                        cases[i].name, cases[i].raw, (long)cases[i].expected_c100, (long)got);
            }
        }
    }

    if (report) {
        cprintf("mcp9808 selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1 ? "" : "s");
    }
    return failed;
}

/* --- Sensor Device-Class Contract --- */

static bool mcp9808_dev_init(struct sensor_dev *dev) {
    if (!mcp9808_init()) return false;
    dev->addr = g.addr;
    dev->bus = g.bus;
    dev->chan_mask = (1u << SENSOR_CHAN_TEMP);
    return true;
}

static bool mcp9808_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    mcp9808_reading_t r;
    return mcp9808_read(&r);
}

static bool mcp9808_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val) {
    (void)dev;
    if (!g.have_last || !out_val) return false;
    if (chan == SENSOR_CHAN_TEMP) {
        *out_val = g.last.temp_c100;
        return true;
    }
    return false;
}

static uint32_t mcp9808_dev_selftest(bool report) {
    return mcp9808_selftest(report);
}

static const sensor_ops_t mcp9808_ops = {
    .init      = mcp9808_dev_init,
    .sample    = mcp9808_dev_sample,
    .get_value = mcp9808_dev_get_value,
    .selftest  = mcp9808_dev_selftest,
};

sensor_dev_t mcp9808_sensor_dev = {
    .name      = "mcp9808",
    .addr      = 0,
    .bus       = 0,
    .chan_mask = (1u << SENSOR_CHAN_TEMP),
    .ops       = &mcp9808_ops,
    .priv      = NULL,
};
