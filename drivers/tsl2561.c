#include "drivers/tsl2561.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <string.h>

/* AMS / TAOS TSL2561 Light-to-Digital Converter.
 * Phase 46, plan/phase46_sensor_framework.md §4.6 (Milestone 46.6).
 *
 * Implements pure integer illuminance (Lux) calculation without FPU instructions.
 */

#define CMD_BYTE          0x80u   /* bit 7 CMD=1 */
#define CMD_WORD          0xA0u   /* bit 7 CMD=1, bit 5 WORD=1 */

#define REG_CONTROL       0x00u
#define REG_TIMING        0x01u
#define REG_ID            0x0Au
#define REG_DATA0LOW      0x0Cu
#define REG_DATA1LOW      0x0Eu

#define CONTROL_POWERON   0x03u
#define CONTROL_POWEROFF  0x00u

/* Scaling factors and piecewise coefficients from TAOS datasheet */
#define LUX_SCALE         14u
#define RATIO_SCALE       9u
#define CH_SCALE          10u

#define CHSCALE_TINT0     0x7517u /* 13.7 ms: 322/11 * 2^CH_SCALE */
#define CHSCALE_TINT1     0x0fe7u /* 101 ms: 322/81 * 2^CH_SCALE */

#define K1T 0x0040u  /* 0.125 * 2^RATIO_SCALE */
#define B1T 0x01f2u  /* 0.0304 * 2^LUX_SCALE */
#define M1T 0x01beu  /* 0.0272 * 2^LUX_SCALE */

#define K2T 0x0080u  /* 0.250 * 2^RATIO_SCALE */
#define B2T 0x0214u  /* 0.0325 * 2^LUX_SCALE */
#define M2T 0x02d1u  /* 0.0440 * 2^LUX_SCALE */

#define K3T 0x00c0u  /* 0.375 * 2^RATIO_SCALE */
#define B3T 0x023fu  /* 0.0351 * 2^LUX_SCALE */
#define M3T 0x037bu  /* 0.0544 * 2^LUX_SCALE */

#define K4T 0x0100u  /* 0.500 * 2^RATIO_SCALE */
#define B4T 0x0270u  /* 0.0381 * 2^LUX_SCALE */
#define M4T 0x03feu  /* 0.0624 * 2^LUX_SCALE */

#define K5T 0x0138u  /* 0.610 * 2^RATIO_SCALE */
#define B5T 0x016fu  /* 0.0224 * 2^LUX_SCALE */
#define M5T 0x01fcu  /* 0.0310 * 2^LUX_SCALE */

#define K6T 0x019au  /* 0.800 * 2^RATIO_SCALE */
#define B6T 0x00d2u  /* 0.0128 * 2^LUX_SCALE */
#define M6T 0x00fbu  /* 0.0153 * 2^LUX_SCALE */

#define K7T 0x029au  /* 1.300 * 2^RATIO_SCALE */
#define B7T 0x0018u  /* 0.00146 * 2^LUX_SCALE */
#define M7T 0x0012u  /* 0.00112 * 2^LUX_SCALE */

static struct {
    bool              detected;
    uint8_t           addr;
    tsl2561_gain_t    gain;
    tsl2561_integ_t   integ;

    tsl2561_reading_t last;
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

bool        tsl2561_is_detected(void)  { return g.detected; }
uint8_t     tsl2561_address(void)      { return g.addr; }
const char *tsl2561_part_name(void)    { return "tsl2561"; }

uint32_t    tsl2561_read_count(void)   { return g.read_count; }
uint32_t    tsl2561_fail_count(void)   { return g.fail_count; }
const char *tsl2561_last_failure(void) { return g.last_fail; }

bool tsl2561_cached(tsl2561_reading_t *out, uint32_t *age_s) {
    if (!out || !g.have_last) return false;
    *out = g.last;
    if (age_s) {
        uint64_t now = time_get_ms();
        *age_s = (now >= g.last_ms) ? (uint32_t)((now - g.last_ms) / 1000u) : 0u;
    }
    return true;
}

int32_t tsl2561_compensate_lux(uint16_t ch0, uint16_t ch1, uint8_t gain, uint8_t integ) {
    if (ch0 == 0) return 0;

    uint32_t ch_scale = (1u << CH_SCALE);
    if (integ == TSL2561_INTEG_13MS) {
        ch_scale = CHSCALE_TINT0;
    } else if (integ == TSL2561_INTEG_101MS) {
        ch_scale = CHSCALE_TINT1;
    }

    if (gain == TSL2561_GAIN_1X) {
        ch_scale <<= 4;
    }

    uint32_t channel0 = ((uint32_t)ch0 * ch_scale) >> CH_SCALE;
    uint32_t channel1 = ((uint32_t)ch1 * ch_scale) >> CH_SCALE;

    if (channel0 == 0) return 0;

    uint32_t ratio1 = ((uint32_t)channel1 << (RATIO_SCALE + 1)) / channel0;
    uint32_t ratio = (ratio1 + 1u) >> 1;

    uint32_t b = 0, m = 0;
    if (ratio <= K1T)      { b = B1T; m = M1T; }
    else if (ratio <= K2T) { b = B2T; m = M2T; }
    else if (ratio <= K3T) { b = B3T; m = M3T; }
    else if (ratio <= K4T) { b = B4T; m = M4T; }
    else if (ratio <= K5T) { b = B5T; m = M5T; }
    else if (ratio <= K6T) { b = B6T; m = M6T; }
    else if (ratio <= K7T) { b = B7T; m = M7T; }
    else return 0;

    uint32_t term0 = channel0 * b;
    uint32_t term1 = channel1 * m;
    if (term0 <= term1) return 0;

    uint32_t diff = term0 - term1;
    uint32_t lux_c100 = (uint32_t)(((uint64_t)diff * 100u + (1u << (LUX_SCALE - 1))) >> LUX_SCALE);
    return (int32_t)lux_c100;
}

bool tsl2561_init(void) {
    static const uint8_t s_addrs[] = {
        TSL2561_ADDR_FLOAT, /* 0x39 */
        TSL2561_ADDR_HIGH,  /* 0x49 */
        TSL2561_ADDR_LOW    /* 0x29 */
    };

    g.detected = false;

    for (uint32_t i = 0; i < sizeof(s_addrs) / sizeof(s_addrs[0]); i++) {
        uint8_t a = s_addrs[i];

        /* If address is 0x29, verify it's not a TSL2591 (chip ID 0x50 at 0x12) */
        if (a == 0x29u) {
            uint8_t tsl2591_id = 0;
            if (i2c_reg_read_u8(0x29u, 0xA0u | 0x12u, &tsl2591_id) && tsl2591_id == 0x50u) {
                continue;
            }
        }

        uint8_t id = 0;
        if (!i2c_reg_read_u8(a, CMD_BYTE | REG_ID, &id)) continue;

        uint8_t part = (id >> 4);
        if (part != 0x01 && part != 0x05) continue;

        /* Verify power control register */
        if (!i2c_reg_write_u8(a, CMD_BYTE | REG_CONTROL, CONTROL_POWERON)) continue;
        uint8_t ctrl = 0;
        if (!i2c_reg_read_u8(a, CMD_BYTE | REG_CONTROL, &ctrl)) continue;
        if ((ctrl & 0x03u) != CONTROL_POWERON) continue;

        /* Power down until sampling */
        (void)i2c_reg_write_u8(a, CMD_BYTE | REG_CONTROL, CONTROL_POWEROFF);

        g.addr = a;
        g.gain = TSL2561_GAIN_16X;
        g.integ = TSL2561_INTEG_101MS;

        /* Configure gain and integration time */
        if (!i2c_reg_write_u8(g.addr, CMD_BYTE | REG_TIMING, (uint8_t)(g.gain | g.integ))) {
            continue;
        }

        g.detected = true;
        printk("[TSL2561] TSL2561 light sensor at 0x%02x on the shared I2C bus.\n", g.addr);
        return true;
    }

    return false;
}

bool tsl2561_read(tsl2561_reading_t *out) {
    if (!out || !g.detected) return false;

    /* Power up device */
    if (!i2c_reg_write_u8(g.addr, CMD_BYTE | REG_CONTROL, CONTROL_POWERON)) {
        return fail("power on write");
    }

    /* Wait for integration cycle (101 ms nominal + 15 ms margin) */
    uint64_t wait_until = time_get_ms() + 120u;
    while (time_get_ms() < wait_until) sched_yield();

    /* Read Channel 0 (broadband) and Channel 1 (IR) */
    uint16_t ch0 = 0, ch1 = 0;
    if (!i2c_reg_read_u16_le(g.addr, CMD_WORD | REG_DATA0LOW, &ch0)) {
        return fail("read ch0");
    }
    if (!i2c_reg_read_u16_le(g.addr, CMD_WORD | REG_DATA1LOW, &ch1)) {
        return fail("read ch1");
    }

    /* Power back down into low-power mode */
    (void)i2c_reg_write_u8(g.addr, CMD_BYTE | REG_CONTROL, CONTROL_POWEROFF);

    out->ch0 = ch0;
    out->ch1 = ch1;
    out->lux_c100 = tsl2561_compensate_lux(ch0, ch1, g.gain, g.integ);

    g.last = *out;
    g.last_ms = time_get_ms();
    g.have_last = true;
    g.read_count++;
    return true;
}

uint32_t tsl2561_selftest(bool report) {
    static const struct {
        uint16_t ch0;
        uint16_t ch1;
        uint8_t  gain;
        uint8_t  integ;
        int32_t  expected_c100;
    } vectors[] = {
        { 1000u,  100u, TSL2561_GAIN_16X, TSL2561_INTEG_402MS,   2767 },
        { 1000u,  200u, TSL2561_GAIN_16X, TSL2561_INTEG_402MS,   2367 },
        { 1000u,  300u, TSL2561_GAIN_16X, TSL2561_INTEG_402MS,   1878 },
        { 1000u,  450u, TSL2561_GAIN_16X, TSL2561_INTEG_402MS,   1002 },
        { 1000u,  550u, TSL2561_GAIN_16X, TSL2561_INTEG_402MS,    535 },
        { 1000u,  700u, TSL2561_GAIN_16X, TSL2561_INTEG_402MS,    209 },
        { 1000u,  900u, TSL2561_GAIN_16X, TSL2561_INTEG_402MS,     48 },
        { 1000u, 1500u, TSL2561_GAIN_16X, TSL2561_INTEG_402MS,      0 },
        {  500u,  100u, TSL2561_GAIN_16X, TSL2561_INTEG_101MS,   4705 },
        { 5000u, 1000u, TSL2561_GAIN_1X,  TSL2561_INTEG_402MS, 189355 },
    };

    uint32_t failed = 0;
    for (uint32_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        int32_t got = tsl2561_compensate_lux(vectors[i].ch0, vectors[i].ch1,
                                            vectors[i].gain, vectors[i].integ);
        if (got != vectors[i].expected_c100) {
            failed++;
            if (report) {
                cprintf("  tsl2561 vector %lu: got %ld, want %ld\n",
                        (unsigned long)i, (long)got, (long)vectors[i].expected_c100);
            }
        }
    }

    if (report) {
        cprintf("tsl2561 selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1u ? "" : "s");
    }
    return failed;
}

void tsl2561_print_status(void) {
    if (!g.detected) {
        cprintf("sensor: tsl2561 not detected\n");
        return;
    }
    tsl2561_reading_t r;
    if (!tsl2561_read(&r)) {
        cprintf("sensor: tsl2561 at 0x%02x read failed: %s (fail count %lu)\n",
                g.addr, g.last_fail ? g.last_fail : "?", (unsigned long)g.fail_count);
        return;
    }
    int32_t w = r.lux_c100 / 100;
    int32_t f = r.lux_c100 % 100;
    cprintf("tsl2561 at 0x%02x: %ld.%02ld Lux (CH0=%u, CH1=%u)\n",
            g.addr, (long)w, (long)f, r.ch0, r.ch1);
}

/* --- Sensor Device Contract (Category D) --- */

static bool tsl2561_dev_init(struct sensor_dev *dev) {
    if (!tsl2561_init()) return false;
    dev->addr = g.addr;
    dev->chan_mask = (1u << SENSOR_CHAN_LUX);
    return true;
}

static bool tsl2561_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    tsl2561_reading_t r;
    return tsl2561_read(&r);
}

static bool tsl2561_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val) {
    (void)dev;
    if (!g.have_last || !out_val) return false;
    if (chan == SENSOR_CHAN_LUX) {
        *out_val = g.last.lux_c100;
        return true;
    }
    return false;
}

static uint32_t tsl2561_dev_selftest(bool report) {
    return tsl2561_selftest(report);
}

static const sensor_ops_t tsl2561_ops = {
    .init      = tsl2561_dev_init,
    .sample    = tsl2561_dev_sample,
    .get_value = tsl2561_dev_get_value,
    .selftest  = tsl2561_dev_selftest,
};

sensor_dev_t tsl2561_sensor_dev = {
    .name      = "tsl2561",
    .addr      = 0,
    .chan_mask = (1u << SENSOR_CHAN_LUX),
    .ops       = &tsl2561_ops,
    .priv      = NULL,
};
