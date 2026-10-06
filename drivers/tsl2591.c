#include "drivers/tsl2591.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <string.h>

/* AMS TSL2591 High Dynamic Range Light Sensor Driver.
 *
 * Implements pure integer illuminance (Lux) calculation without FPU instructions.
 */

#define CMD_NORMAL_OP     0xA0u   /* bit 7 CMD=1, bits 6:5 TRANSACTION=01 (normal) */

#define REG_ENABLE        0x00u
#define REG_CONFIG        0x01u
#define REG_PID           0x11u
#define REG_ID            0x12u
#define REG_STATUS        0x13u
#define REG_C0DATAL       0x14u

#define ENABLE_PON        (1u << 0)
#define ENABLE_AEN        (1u << 1)
#define STATUS_AVALID     (1u << 0)

#define LUX_COEFF         408u

static struct {
    bool              detected;
    uint8_t           addr;
    tsl2591_gain_t    gain;
    tsl2591_integ_t   integ;

    tsl2591_reading_t last;
    uint64_t          last_ms;
    bool              have_last;

    uint32_t          read_count;
    uint32_t          fail_count;
    const char       *last_fail;
} g;

static bool rd(uint8_t reg, uint8_t *dst, uint32_t len) {
    return i2c_reg_read_bytes(g.addr, CMD_NORMAL_OP | reg, dst, len);
}

static bool wr(uint8_t reg, uint8_t value) {
    return i2c_reg_write_u8(g.addr, CMD_NORMAL_OP | reg, value);
}

static bool fail(const char *where) {
    g.last_fail = where;
    g.fail_count++;
    return false;
}

bool        tsl2591_is_detected(void)  { return g.detected; }
uint8_t     tsl2591_address(void)      { return g.addr; }
const char *tsl2591_part_name(void)    { return "tsl2591"; }

uint32_t    tsl2591_read_count(void)   { return g.read_count; }
uint32_t    tsl2591_fail_count(void)   { return g.fail_count; }
const char *tsl2591_last_failure(void) { return g.last_fail; }

bool tsl2591_cached(tsl2591_reading_t *out, uint32_t *age_s) {
    if (!out || !g.have_last) return false;
    *out = g.last;
    if (age_s) {
        uint64_t now = time_get_ms();
        *age_s = (now >= g.last_ms) ? (uint32_t)((now - g.last_ms) / 1000u) : 0u;
    }
    return true;
}

int32_t tsl2591_compensate_lux(uint16_t ch0, uint16_t ch1, uint16_t gain_mult, uint16_t int_time_ms) {
    if (ch0 == 0 || ch0 <= ch1) return 0;

    /* Guard against saturation */
    if (ch0 >= 0xFFFFu || ch1 >= 0xFFFFu) {
        ch0 = 0xFFFFu;
        if (ch1 >= ch0) return 0;
    }

    uint32_t cpl = ((uint32_t)int_time_ms * (uint32_t)gain_mult) / LUX_COEFF;
    if (cpl == 0) cpl = 1u;

    uint32_t ratio = ((uint32_t)ch1 * 1000u) / (uint32_t)ch0;
    if (ratio > 1000u) ratio = 1000u;

    uint32_t factor = 1000u - ratio;
    uint32_t diff = (uint32_t)(ch0 - ch1);
    uint32_t lux_m = (diff * factor) / cpl;

    return (int32_t)(lux_m / 10u);
}

bool tsl2591_init(void) {
    g.addr = TSL2591_ADDR;
    uint8_t id = 0;
    if (!rd(REG_ID, &id, 1u)) {
        g.detected = false;
        return false;
    }
    if (id != TSL2591_CHIP_ID) {
        g.detected = false;
        return false;
    }

    g.gain = TSL2591_GAIN_MED;      /* 25x gain */
    g.integ = TSL2591_INTEG_100MS;  /* 100 ms integration time */

    /* Configure gain and integration time */
    if (!wr(REG_CONFIG, (uint8_t)(g.gain | g.integ))) return false;

    /* Power off until an on-demand reading is requested */
    (void)wr(REG_ENABLE, 0x00u);

    g.detected = true;
    printk("[TSL2591] TSL2591 light sensor at 0x%02x on the shared I2C bus.\n", g.addr);
    return true;
}

bool tsl2591_read(tsl2591_reading_t *out) {
    if (!out || !g.detected) return false;

    /* Power on and enable ALS conversion */
    if (!wr(REG_ENABLE, ENABLE_PON | ENABLE_AEN)) return fail("enable write");

    /* Wait for integration cycle (100 ms nominal + 15 ms margin) */
    uint64_t wait_until = time_get_ms() + 115u;
    while (time_get_ms() < wait_until) sched_yield();

    /* Poll for AVALID bit in STATUS register (up to 100 ms timeout) */
    uint64_t deadline = time_get_ms() + 100u;
    for (;;) {
        uint8_t status = 0;
        if (!rd(REG_STATUS, &status, 1u)) return fail("status poll");
        if (status & STATUS_AVALID) break;
        if (time_get_ms() >= deadline) return fail("integration timeout");
        sched_yield();
    }

    /* Read 4 bytes: C0DATAL, C0DATAH, C1DATAL, C1DATAH */
    uint8_t d[4];
    if (!rd(REG_C0DATAL, d, 4u)) return fail("adc data read");

    /* Power back down into low-power sleep state (3 uA) */
    (void)wr(REG_ENABLE, 0x00u);

    uint16_t ch0 = (uint16_t)((uint16_t)d[1] << 8 | d[0]);
    uint16_t ch1 = (uint16_t)((uint16_t)d[3] << 8 | d[2]);

    out->ch0 = ch0;
    out->ch1 = ch1;
    out->lux_c100 = tsl2591_compensate_lux(ch0, ch1, 25u, 100u);

    g.last = *out;
    g.last_ms = time_get_ms();
    g.have_last = true;
    g.read_count++;
    return true;
}

uint32_t tsl2591_selftest(bool report) {
    uint32_t failed = 0;

    int32_t v1 = tsl2591_compensate_lux(2000u, 500u, 25u, 100u);
    if (v1 != 18750) {
        failed++;
        if (report) cprintf("  tsl2591 vector 1: got %ld, want 18750\n", (long)v1);
    }

    int32_t v2 = tsl2591_compensate_lux(5000u, 1200u, 25u, 100u);
    if (v2 != 48133) {
        failed++;
        if (report) cprintf("  tsl2591 vector 2: got %ld, want 48133\n", (long)v2);
    }

    if (report) cprintf("tsl2591 selftest: %lu case%s failed\n",
                        (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}

void tsl2591_print_status(void) {
    if (!g.detected) {
        cprintf("sensor: tsl2591 not detected\n");
        return;
    }
    tsl2591_reading_t r;
    if (!tsl2591_read(&r)) {
        cprintf("sensor: tsl2591 at 0x%02x read failed: %s (fail count %lu)\n",
                g.addr, g.last_fail ? g.last_fail : "?", (unsigned long)g.fail_count);
        return;
    }
    int32_t w = r.lux_c100 / 100;
    int32_t f = r.lux_c100 % 100;
    cprintf("tsl2591 at 0x%02x: %ld.%02ld Lux (CH0=%u, CH1=%u)\n",
            g.addr, (long)w, (long)f, r.ch0, r.ch1);
}

/* --- Sensor Device Contract (Category D) --- */

static bool tsl2591_dev_init(struct sensor_dev *dev) {
    if (!tsl2591_init()) return false;
    dev->addr = g.addr;
    dev->chan_mask = (1u << SENSOR_CHAN_LUX);
    return true;
}

static bool tsl2591_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    tsl2591_reading_t r;
    return tsl2591_read(&r);
}

static bool tsl2591_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val) {
    (void)dev;
    if (!g.have_last || !out_val) return false;
    if (chan == SENSOR_CHAN_LUX) {
        *out_val = g.last.lux_c100;
        return true;
    }
    return false;
}

static uint32_t tsl2591_dev_selftest(bool report) {
    return tsl2591_selftest(report);
}

static const sensor_ops_t tsl2591_ops = {
    .init      = tsl2591_dev_init,
    .sample    = tsl2591_dev_sample,
    .get_value = tsl2591_dev_get_value,
    .selftest  = tsl2591_dev_selftest,
};

sensor_dev_t tsl2591_sensor_dev = {
    .name      = "tsl2591",
    .addr      = 0,
    .chan_mask = (1u << SENSOR_CHAN_LUX),
    .ops       = &tsl2591_ops,
    .priv      = NULL,
};
