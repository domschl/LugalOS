#include "drivers/ina226.h"
#include "drivers/battery.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "lugalos_config.h"
#include <string.h>

#define MODULE_SHUNT_UOHM 100000u   /* the R100 on common INA226 breakouts */

static struct {
    bool             detected;
    uint8_t          bus, addr;
    bool             board_part;    /* the board's own monitor, on its battery */
    uint32_t         shunt_uohm;
    bool             invert;
    ina226_reading_t last;
    bool             have_last;
} g;

bool ina226_is_detected(void) { return g.detected; }

int32_t ina226_calc_mv(uint16_t bus_raw) {
    return (int32_t)(((uint32_t)bus_raw * 5u + 2u) / 4u);
}

/* nV / uOhm = mA: 2500 nV per LSB over the shunt, rounded half away from 0 */
int32_t ina226_calc_ma(int16_t shunt_raw, uint32_t shunt_uohm) {
    if (shunt_uohm == 0) return 0;
    int64_t nv = (int64_t)shunt_raw * 2500;
    int64_t half = (int64_t)(shunt_uohm / 2u);
    return (int32_t)(nv >= 0 ? (nv + half) / shunt_uohm : (nv - half) / (int64_t)shunt_uohm);
}

static bool ina226_init(void) {
    uint8_t buses = i2c_bus_count();
    for (uint8_t b = 0; b < buses; b++) {
        for (uint8_t addr = 0x40u; addr <= 0x4Fu; addr++) {
            uint16_t mfg = 0, die = 0;
            if (!i2c_reg_read_u16_be_bus(b, addr, INA226_REG_MFG_ID, &mfg)) continue;
            if (mfg != INA226_MFG_ID) continue;
            if (!i2c_reg_read_u16_be_bus(b, addr, INA226_REG_DIE_ID, &die)) continue;
            if (die != INA226_DIE_ID) continue;
            if (!i2c_reg_write_u16_be_bus(b, addr, INA226_REG_CONFIG, INA226_CONFIG)) continue;

            const char *part = i2c_bus_part(b, addr);
            g.board_part = part && strcmp(part, "ina226") == 0;
#if defined(CONFIG_INA226_SHUNT_UOHM)
            g.shunt_uohm = g.board_part ? CONFIG_INA226_SHUNT_UOHM : MODULE_SHUNT_UOHM;
#else
            g.shunt_uohm = MODULE_SHUNT_UOHM;
#endif
#if defined(CONFIG_INA226_INVERT)
            g.invert = g.board_part && CONFIG_INA226_INVERT;
#endif
            g.detected = true;
            g.bus = b;
            g.addr = addr;
            printk("[INA226] power monitor at bus %u 0x%02x, %lu mOhm shunt%s\n",
                   b, addr, (unsigned long)(g.shunt_uohm / 1000u),
                   g.board_part ? " (the board's battery)" : "");
            return true;
        }
    }
    g.detected = false;
    return false;
}

bool ina226_read(ina226_reading_t *out) {
    if (!g.detected) return false;
    uint16_t sh = 0, bus = 0;
    if (!i2c_reg_read_u16_be_bus(g.bus, g.addr, INA226_REG_SHUNT, &sh)) return false;
    if (!i2c_reg_read_u16_be_bus(g.bus, g.addr, INA226_REG_BUS, &bus)) return false;
    ina226_reading_t r;
    r.mv = ina226_calc_mv(bus);
    r.ma = ina226_calc_ma((int16_t)sh, g.shunt_uohm);
    if (g.invert) r.ma = -r.ma;
    r.mw = (int32_t)(((int64_t)r.mv * r.ma) / 1000);
    g.last = r;
    g.have_last = true;
    if (g.board_part && battery_board()) battery_update(battery_board(), r.mv, r.ma, time_get_ms());
    if (out) *out = r;
    return true;
}

uint32_t ina226_selftest(bool report) {
    static const struct { uint16_t raw; int32_t mv; } bus[] = {
        { 0, 0 }, { 1, 1 }, { 4, 5 }, { 6289, 7861 }, { 6584, 8230 }, { 0x7fff, 40959 },
    };
    static const struct { int16_t raw; uint32_t uohm; int32_t ma; } sh[] = {
        { 0, 5000, 0 }, { 1000, 5000, 500 }, { -824, 5000, -412 }, { 1, 5000, 1 },
        { -1, 5000, -1 }, { 32767, 5000, 16384 }, { 400, 100000, 10 },
    };
    uint32_t failed = 0;
    for (unsigned i = 0; i < sizeof(bus) / sizeof(bus[0]); i++) {
        int32_t got = ina226_calc_mv(bus[i].raw);
        if (got != bus[i].mv) {
            failed++;
            if (report) cprintf("  ina226 bus %u: raw %u -> %ld mV, want %ld\n", i,
                                (unsigned)bus[i].raw, (long)got, (long)bus[i].mv);
        }
    }
    for (unsigned i = 0; i < sizeof(sh) / sizeof(sh[0]); i++) {
        int32_t got = ina226_calc_ma(sh[i].raw, sh[i].uohm);
        if (got != sh[i].ma) {
            failed++;
            if (report) cprintf("  ina226 shunt %u: raw %d -> %ld mA, want %ld\n", i,
                                (int)sh[i].raw, (long)got, (long)sh[i].ma);
        }
    }
    if (report) cprintf("ina226 selftest: %lu case%s failed\n",
                        (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}

/* --- Sensor Device-Class Contract --- */

static bool ina226_dev_init(struct sensor_dev *dev) {
    if (!ina226_init()) return false;
    dev->bus = g.bus;
    dev->addr = g.addr;
    dev->chan_mask = (1u << SENSOR_CHAN_VOLTAGE) | (1u << SENSOR_CHAN_CURRENT) |
                     (1u << SENSOR_CHAN_POWER);
    if (g.board_part && battery_board()) dev->chan_mask |= (1u << SENSOR_CHAN_BATTERY);
    return true;
}

static bool ina226_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    return ina226_read(NULL);
}

static bool ina226_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out) {
    (void)dev;
    if (!g.have_last || !out) return false;
    switch (chan) {
    case SENSOR_CHAN_VOLTAGE: *out = g.last.mv; return true;
    case SENSOR_CHAN_CURRENT: *out = g.last.ma; return true;
    case SENSOR_CHAN_POWER:   *out = g.last.mw; return true;
    case SENSOR_CHAN_BATTERY: return g.board_part && battery_soc(battery_board(), out);
    default:                  return false;
    }
}

static const sensor_ops_t ina226_ops = {
    .init      = ina226_dev_init,
    .sample    = ina226_dev_sample,
    .get_value = ina226_dev_get_value,
    .selftest  = ina226_selftest,
};

/* Sampled once a second: the charge count integrates current over time, and
 * the 35 ms average the chip keeps is what each sample reads. */
sensor_dev_t ina226_sensor_dev = {
    .name      = "ina226",
    .chan_mask = (1u << SENSOR_CHAN_VOLTAGE) | (1u << SENSOR_CHAN_CURRENT) | (1u << SENSOR_CHAN_POWER),
    .period_ms = 1000,
    .ops       = &ina226_ops,
};
