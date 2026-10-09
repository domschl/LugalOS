#include "drivers/ccs811.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <string.h>

/* AMS CCS811 Ultra-Low Power Digital Gas Sensor Driver.
 * Phase 46, plan/phase46_sensor_framework.md §4.7 & §4.9.
 *
 * Implements:
 * - Bootloader to application state transition (APP_START 0xF4)
 * - Drive mode configuration (Mode 1: 1-second continuous IAQ)
 * - Algorithm results extraction: eCO2 (ppm) and TVOC (ppb)
 * - Cross-sensor environmental baseline compensation via ENV_DATA (reg 0x05)
 */

#define REG_STATUS           0x00u
#define REG_MEAS_MODE        0x01u
#define REG_ALG_RESULT_DATA  0x02u
#define REG_RAW_DATA         0x03u
#define REG_ENV_DATA         0x05u
#define REG_BASELINE         0x11u
#define REG_HW_ID            0x20u
#define REG_ERROR_ID         0xE0u
#define REG_APP_START        0xF4u
#define REG_SW_RESET         0xFFu

#define STATUS_FW_MODE       0x80u /* 1 = application mode, 0 = boot mode */
#define STATUS_APP_VALID     0x10u /* 1 = valid application firmware */
#define STATUS_DATA_READY    0x08u /* 1 = new sample ready */
#define STATUS_ERROR         0x01u /* 1 = error in ERROR_ID */

static struct {
    bool                detected;
    uint8_t             addr;
    ccs811_drive_mode_t mode;

    ccs811_reading_t    last;
    uint64_t            last_ms;
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

bool        ccs811_is_detected(void)  { return g.detected; }
uint8_t     ccs811_address(void)      { return g.addr; }
const char *ccs811_part_name(void)    { return "ccs811"; }

uint32_t    ccs811_read_count(void)   { return g.read_count; }
uint32_t    ccs811_fail_count(void)   { return g.fail_count; }
const char *ccs811_last_failure(void) { return g.last_fail; }

bool ccs811_cached(ccs811_reading_t *out, uint32_t *age_s) {
    if (!out || !g.have_last) return false;
    *out = g.last;
    if (age_s) {
        uint64_t now = time_get_ms();
        *age_s = (now >= g.last_ms) ? (uint32_t)((now - g.last_ms) / 1000u) : 0u;
    }
    return true;
}

void ccs811_encode_env_data(int32_t temp_c100, int32_t rh_cpercent, uint8_t out[4]) {
    if (rh_cpercent < 0) rh_cpercent = 0;
    if (rh_cpercent > 10000) rh_cpercent = 10000;
    uint32_t rh_raw = (uint32_t)((rh_cpercent * 512 + 50) / 100);

    int32_t t_offset = temp_c100 + 2500;
    if (t_offset < 0) t_offset = 0;
    uint32_t temp_raw = (uint32_t)((t_offset * 512 + 50) / 100);

    out[0] = (uint8_t)((rh_raw >> 8) & 0xFF);
    out[1] = (uint8_t)(rh_raw & 0xFF);
    out[2] = (uint8_t)((temp_raw >> 8) & 0xFF);
    out[3] = (uint8_t)(temp_raw & 0xFF);
}

bool ccs811_set_env_data(int32_t temp_c100, int32_t rh_cpercent) {
    if (!g.detected) return false;
    uint8_t payload[4];
    ccs811_encode_env_data(temp_c100, rh_cpercent, payload);
    return i2c_reg_write_bytes(g.addr, REG_ENV_DATA, payload, 4);
}

bool ccs811_get_baseline(uint16_t *baseline_out) {
    if (!g.detected || !baseline_out) return false;
    uint8_t buf[2];
    if (!i2c_reg_read_bytes(g.addr, REG_BASELINE, buf, 2)) return false;
    *baseline_out = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    return true;
}

bool ccs811_set_baseline(uint16_t baseline) {
    if (!g.detected) return false;
    uint8_t buf[2] = { (uint8_t)(baseline >> 8), (uint8_t)(baseline & 0xFF) };
    return i2c_reg_write_bytes(g.addr, REG_BASELINE, buf, 2);
}

bool ccs811_reset(void) {
    if (!g.detected) return false;
    static const uint8_t rst_cmd[4] = { 0x11, 0xE5, 0x72, 0x8A };
    (void)i2c_reg_write_bytes(g.addr, REG_SW_RESET, rst_cmd, 4);
    time_delay_us(10000); /* 10 ms wait for bootloader reset */
    return ccs811_init();
}

bool ccs811_init(void) {
    static const uint8_t s_addrs[] = { CCS811_ADDR_LOW, CCS811_ADDR_HIGH };
    g.detected = false;

    for (uint32_t i = 0; i < sizeof(s_addrs) / sizeof(s_addrs[0]); i++) {
        uint8_t a = s_addrs[i];

        uint8_t hw_id = 0;
        if (!i2c_reg_read_u8(a, REG_HW_ID, &hw_id)) continue;
        if (hw_id != CCS811_HW_ID) continue;

        uint8_t status = 0;
        if (!i2c_reg_read_u8(a, REG_STATUS, &status)) continue;

        /* If in bootloader mode, start application */
        if (!(status & STATUS_FW_MODE)) {
            if (!(status & STATUS_APP_VALID)) {
                continue;
            }
            /* Write to APP_START register (0xF4) with 0-byte body */
            if (!i2c_reg_write_bytes(a, REG_APP_START, NULL, 0)) {
                continue;
            }
            /* Wait 2 ms for state machine transition */
            uint64_t wait_until = time_get_ms() + 2u;
            while (time_get_ms() < wait_until) sched_yield();

            if (!i2c_reg_read_u8(a, REG_STATUS, &status)) continue;
            if (!(status & STATUS_FW_MODE)) {
                continue;
            }
        }

        /* Configure measurement mode: Mode 1 (every 1s) */
        g.addr = a;
        g.mode = CCS811_DRIVE_MODE_1S;
        if (!i2c_reg_write_u8(g.addr, REG_MEAS_MODE, (uint8_t)g.mode)) {
            continue;
        }

        g.detected = true;
        printk("[CCS811] CCS811 MOX air quality sensor at 0x%02x on the shared I2C bus.\n", g.addr);
        return true;
    }

    return false;
}

bool ccs811_read(ccs811_reading_t *out) {
    if (!out || !g.detected) return false;

    uint8_t buf[5];
    if (!i2c_reg_read_bytes(g.addr, REG_ALG_RESULT_DATA, buf, 5)) {
        return fail("alg result read");
    }

    uint16_t eco2 = (uint16_t)((uint16_t)buf[0] << 8 | buf[1]);
    uint16_t tvoc = (uint16_t)((uint16_t)buf[2] << 8 | buf[3]);
    uint8_t status = buf[4];

    if (status & STATUS_ERROR) {
        uint8_t err = 0;
        (void)i2c_reg_read_u8(g.addr, REG_ERROR_ID, &err);
        out->error_id = err;
        return fail("status error");
    }

    /* Until the first conversions complete after APP_START or a reset, the
     * result registers hold 0/0 -- with DATA_READY clear, and seen on the
     * first boards for a sample or two with it set. eCO2 is clipped to
     * 400..8192 ppm (datasheet, "eCO2"), so 0 is never a measurement. Caching
     * it would publish 0 ppm eCO2 as a reading (and as the fused CO2
     * fallback), so such a read keeps the previous sample instead. */
    if (!(status & STATUS_DATA_READY) || eco2 == 0) {
        if (!g.have_last) return false;
        *out = g.last;
        return true;
    }

    out->eco2_ppm = eco2;
    out->tvoc_ppb = tvoc;
    out->status = status;
    out->error_id = 0;

    g.last = *out;
    g.last_ms = time_get_ms();
    g.have_last = true;
    g.read_count++;
    return true;
}

uint32_t ccs811_selftest(bool report) {
    static const struct {
        int32_t temp_c100;
        int32_t rh_cpercent;
        uint8_t expected[4];
    } vectors[] = {
        {  2500, 5000, { 0x64, 0x00, 0x64, 0x00 } },
        {  2350, 4850, { 0x61, 0x00, 0x61, 0x00 } },
        {  2000, 4000, { 0x50, 0x00, 0x5A, 0x00 } },
        {     0,    0, { 0x00, 0x00, 0x32, 0x00 } },
        { -2500,    0, { 0x00, 0x00, 0x00, 0x00 } },
        {  2680, 5230, { 0x68, 0x9A, 0x67, 0x9A } },
    };

    uint32_t failed = 0;
    for (uint32_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint8_t enc[4];
        ccs811_encode_env_data(vectors[i].temp_c100, vectors[i].rh_cpercent, enc);
        if (memcmp(enc, vectors[i].expected, 4) != 0) {
            failed++;
            if (report) {
                cprintf("  ccs811 vector %lu: got %02x%02x%02x%02x, want %02x%02x%02x%02x\n",
                        (unsigned long)i,
                        enc[0], enc[1], enc[2], enc[3],
                        vectors[i].expected[0], vectors[i].expected[1],
                        vectors[i].expected[2], vectors[i].expected[3]);
            }
        }
    }

    if (report) {
        cprintf("ccs811 selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1u ? "" : "s");
    }
    return failed;
}

void ccs811_print_status(void) {
    if (!g.detected) {
        cprintf("sensor: ccs811 not detected\n");
        return;
    }
    ccs811_reading_t r;
    if (!ccs811_read(&r)) {
        cprintf("sensor: ccs811 at 0x%02x read failed: %s (fail count %lu)\n",
                g.addr, g.last_fail ? g.last_fail : "?", (unsigned long)g.fail_count);
        return;
    }
    cprintf("ccs811 at 0x%02x: %u ppm eCO2, %u ppb TVOC (status 0x%02x)\n",
            g.addr, r.eco2_ppm, r.tvoc_ppb, r.status);
}

/* --- Sensor Device Contract (Category D) --- */

static bool ccs811_dev_init(struct sensor_dev *dev) {
    if (!ccs811_init()) return false;
    dev->addr = g.addr;
    dev->chan_mask = (1u << SENSOR_CHAN_ECO2) | (1u << SENSOR_CHAN_TVOC);
    return true;
}

static bool ccs811_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    ccs811_reading_t r;
    return ccs811_read(&r);
}

static bool ccs811_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val) {
    (void)dev;
    if (!g.have_last || !out_val) return false;
    if (chan == SENSOR_CHAN_ECO2) {
        *out_val = (int32_t)g.last.eco2_ppm;
        return true;
    }
    if (chan == SENSOR_CHAN_TVOC) {
        *out_val = (int32_t)g.last.tvoc_ppb;
        return true;
    }
    return false;
}

static uint32_t ccs811_dev_selftest(bool report) {
    return ccs811_selftest(report);
}

static const sensor_ops_t ccs811_ops = {
    .init      = ccs811_dev_init,
    .sample    = ccs811_dev_sample,
    .get_value = ccs811_dev_get_value,
    .selftest  = ccs811_dev_selftest,
};

sensor_dev_t ccs811_sensor_dev = {
    .name      = "ccs811",
    .addr      = 0,
    .chan_mask = (1u << SENSOR_CHAN_ECO2) | (1u << SENSOR_CHAN_TVOC),
    .ops       = &ccs811_ops,
    .priv      = NULL,
};
