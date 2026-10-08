#include "drivers/sgp30.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <string.h>

/* Sensirion SGP30 Multi-Pixel Gas Sensor Driver.
 * Phase 46, plan/phase46_sensor_framework.md §4.8 (Milestone 46.8).
 *
 * Implements:
 * - 16-bit command protocol and CRC-8 validation (Sensirion polynomial 0x31)
 * - Measurement extraction: eCO2 (ppm) and TVOC (ppb)
 * - Cross-sensor absolute humidity compensation (fixed-point 8.8 g/m^3)
 */

#define CMD_IAQ_INIT           0x2003u
#define CMD_MEASURE_IAQ        0x2008u
#define CMD_GET_IAQ_BASELINE   0x2015u
#define CMD_SET_IAQ_BASELINE   0x201Eu
#define CMD_SET_ABSHUM         0x2061u
#define CMD_GET_FEATURE_SET    0x202Fu
#define CMD_GET_SERIAL_ID      0x3682u

static struct {
    bool             detected;
    uint8_t          addr;
    uint16_t         feature_set;

    sgp30_reading_t  last;
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

bool        sgp30_is_detected(void)  { return g.detected; }
uint8_t     sgp30_address(void)      { return g.addr; }
const char *sgp30_part_name(void)    { return "sgp30"; }

uint32_t    sgp30_read_count(void)   { return g.read_count; }
uint32_t    sgp30_fail_count(void)   { return g.fail_count; }
const char *sgp30_last_failure(void) { return g.last_fail; }

bool sgp30_cached(sgp30_reading_t *out, uint32_t *age_s) {
    if (!out || !g.have_last) return false;
    *out = g.last;
    if (age_s) {
        uint64_t now = time_get_ms();
        *age_s = (now >= g.last_ms) ? (uint32_t)((now - g.last_ms) / 1000u) : 0u;
    }
    return true;
}

uint8_t sgp30_crc8(const uint8_t *data, uint32_t len) {
    uint8_t crc = 0xFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 8; bit > 0; --bit) {
            if (crc & 0x80u) {
                crc = (uint8_t)((crc << 1) ^ 0x31u);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

static uint16_t sgp30_calc_ah_8_8(int32_t temp_c100, int32_t rh_cpercent) {
    if (rh_cpercent <= 0 || temp_c100 < -4000) return 0;

    int32_t t_m = temp_c100;
    int64_t x_q16 = ((int64_t)1762 * t_m * 65536) / ((int64_t)(24312 + t_m) * 100);

    int64_t e = 65536 + x_q16;
    int64_t t2 = (x_q16 * x_q16) >> 17;
    e += t2;
    int64_t t3 = (t2 * x_q16) / (3 * 65536);
    e += t3;
    int64_t t4 = (t3 * x_q16) / (4 * 65536);
    e += t4;
    int64_t t5 = (t4 * x_q16) / (5 * 65536);
    e += t5;
    int64_t t6 = (t5 * x_q16) / (6 * 65536);
    e += t6;

    int64_t num = (int64_t)21670 * 6112 * e * rh_cpercent;
    int64_t den = (int64_t)(27315 + t_m) * 256 * 10000000;
    int64_t dv_8_8 = (num + (den / 2)) / den;
    if (dv_8_8 > 0xFFFF) dv_8_8 = 0xFFFF;
    return (uint16_t)dv_8_8;
}

bool sgp30_set_absolute_humidity(int32_t temp_c100, int32_t rh_cpercent) {
    if (!g.detected) return false;
    uint16_t ah = sgp30_calc_ah_8_8(temp_c100, rh_cpercent);

    uint8_t payload[5];
    payload[0] = (uint8_t)(CMD_SET_ABSHUM >> 8);
    payload[1] = (uint8_t)(CMD_SET_ABSHUM & 0xFFu);
    payload[2] = (uint8_t)(ah >> 8);
    payload[3] = (uint8_t)(ah & 0xFFu);
    payload[4] = sgp30_crc8(&payload[2], 2u);

    return i2c_xfer(g.addr, payload, 5u, NULL, 0u);
}

bool sgp30_init(void) {
    g.detected = false;
    g.addr = SGP30_ADDR;

    /* Probe SGP30 by querying feature set (0x202F) */
    uint8_t cmd_feat[2] = { (uint8_t)(CMD_GET_FEATURE_SET >> 8), (uint8_t)(CMD_GET_FEATURE_SET & 0xFFu) };
    uint8_t resp_feat[3];

    if (!i2c_xfer(g.addr, cmd_feat, 2u, NULL, 0u)) {
        return false;
    }

    uint64_t wait_until = time_get_ms() + 10u;
    while (time_get_ms() < wait_until) sched_yield();

    if (!i2c_xfer(g.addr, NULL, 0u, resp_feat, 3u)) {
        return false;
    }

    /* Verify CRC-8 on feature set response */
    if (sgp30_crc8(resp_feat, 2u) != resp_feat[2]) {
        return false;
    }

    g.feature_set = (uint16_t)((uint16_t)resp_feat[0] << 8 | resp_feat[1]);

    /* Send sgp30_iaq_init command (0x2003) */
    uint8_t cmd_init[2] = { (uint8_t)(CMD_IAQ_INIT >> 8), (uint8_t)(CMD_IAQ_INIT & 0xFFu) };
    if (!i2c_xfer(g.addr, cmd_init, 2u, NULL, 0u)) {
        return false;
    }

    wait_until = time_get_ms() + 10u;
    while (time_get_ms() < wait_until) sched_yield();

    g.detected = true;
    printk("[SGP30] SGP30 multi-pixel gas sensor at 0x%02x (feature set 0x%04x).\n",
           g.addr, g.feature_set);
    return true;
}

bool sgp30_read(sgp30_reading_t *out) {
    if (!out || !g.detected) return false;

    /* Issue sgp30_measure_iaq command (0x2008) */
    uint8_t cmd[2] = { (uint8_t)(CMD_MEASURE_IAQ >> 8), (uint8_t)(CMD_MEASURE_IAQ & 0xFFu) };
    if (!i2c_xfer(g.addr, cmd, 2u, NULL, 0u)) {
        return fail("measure command");
    }

    /* Wait for conversion completion (max 12 ms) */
    uint64_t wait_until = time_get_ms() + 14u;
    while (time_get_ms() < wait_until) sched_yield();

    /* Read 6 bytes: eCO2 (2B) + CRC (1B), TVOC (2B) + CRC (1B) */
    uint8_t buf[6];
    if (!i2c_xfer(g.addr, NULL, 0u, buf, 6u)) {
        return fail("read result");
    }

    /* Validate CRCs */
    if (sgp30_crc8(buf, 2u) != buf[2]) {
        return fail("eco2 crc mismatch");
    }
    if (sgp30_crc8(buf + 3, 2u) != buf[5]) {
        return fail("tvoc crc mismatch");
    }

    out->eco2_ppm = (uint16_t)((uint16_t)buf[0] << 8 | buf[1]);
    out->tvoc_ppb = (uint16_t)((uint16_t)buf[3] << 8 | buf[4]);
    out->feature_set = g.feature_set;

    g.last = *out;
    g.last_ms = time_get_ms();
    g.have_last = true;
    g.read_count++;
    return true;
}

bool sgp30_get_baseline(uint16_t *eco2_base, uint16_t *tvoc_base) {
    if (!g.detected || !eco2_base || !tvoc_base) return false;
    uint8_t cmd[2] = { (uint8_t)(CMD_GET_IAQ_BASELINE >> 8), (uint8_t)(CMD_GET_IAQ_BASELINE & 0xFFu) };
    if (!i2c_xfer(g.addr, cmd, 2u, NULL, 0u)) return false;

    uint64_t wait_until = time_get_ms() + 12u;
    while (time_get_ms() < wait_until) sched_yield();

    uint8_t buf[6];
    if (!i2c_xfer(g.addr, NULL, 0u, buf, 6u)) return false;

    if (sgp30_crc8(buf, 2u) != buf[2] || sgp30_crc8(&buf[3], 2u) != buf[5]) return false;

    *eco2_base = (uint16_t)((uint16_t)buf[0] << 8 | buf[1]);
    *tvoc_base = (uint16_t)((uint16_t)buf[3] << 8 | buf[4]);
    return true;
}

bool sgp30_set_baseline(uint16_t eco2_base, uint16_t tvoc_base) {
    if (!g.detected) return false;
    /* Per Sensirion SGP30 datasheet, CMD_SET_IAQ_BASELINE sends TVOC first, then eCO2 */
    uint8_t cmd[8];
    cmd[0] = (uint8_t)(CMD_SET_IAQ_BASELINE >> 8);
    cmd[1] = (uint8_t)(CMD_SET_IAQ_BASELINE & 0xFFu);
    cmd[2] = (uint8_t)(tvoc_base >> 8);
    cmd[3] = (uint8_t)(tvoc_base & 0xFFu);
    cmd[4] = sgp30_crc8(&cmd[2], 2u);
    cmd[5] = (uint8_t)(eco2_base >> 8);
    cmd[6] = (uint8_t)(eco2_base & 0xFFu);
    cmd[7] = sgp30_crc8(&cmd[5], 2u);

    if (!i2c_xfer(g.addr, cmd, 8u, NULL, 0u)) return false;

    uint64_t wait_until = time_get_ms() + 10u;
    while (time_get_ms() < wait_until) sched_yield();
    return true;
}

uint32_t sgp30_selftest(bool report) {
    uint32_t failed = 0;

    /* Test 1: Datasheet CRC vector CRC(0xBEEF) == 0x92 */
    static const uint8_t beef[2] = { 0xBEu, 0xEFu };
    uint8_t c1 = sgp30_crc8(beef, 2u);
    if (c1 != 0x92u) {
        failed++;
        if (report) cprintf("  sgp30 crc test 1: got 0x%02x, want 0x92\n", c1);
    }

    /* Test 2: CRC of eCO2 400 (0x0190) == 0x4C */
    static const uint8_t eco2[2] = { 0x01u, 0x90u };
    uint8_t c2 = sgp30_crc8(eco2, 2u);
    if (c2 != 0x4Cu) {
        failed++;
        if (report) cprintf("  sgp30 crc test 2: got 0x%02x, want 0x4C\n", c2);
    }

    /* Test 3: CRC of TVOC 0 (0x0000) == 0x81 */
    static const uint8_t tvoc[2] = { 0x00u, 0x00u };
    uint8_t c3 = sgp30_crc8(tvoc, 2u);
    if (c3 != 0x81u) {
        failed++;
        if (report) cprintf("  sgp30 crc test 3: got 0x%02x, want 0x81\n", c3);
    }

    /* Test 4: Absolute humidity calculation at 25.0 C, 50.0 %RH */
    uint16_t ah = sgp30_calc_ah_8_8(2500, 5000);
    if (ah != 0x0B77u) {
        failed++;
        if (report) cprintf("  sgp30 ah test: got 0x%04x, want 0x0B77\n", ah);
    }

    if (report) {
        cprintf("sgp30 selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1u ? "" : "s");
    }
    return failed;
}

void sgp30_print_status(void) {
    if (!g.detected) {
        cprintf("sensor: sgp30 not detected\n");
        return;
    }
    sgp30_reading_t r;
    if (!sgp30_read(&r)) {
        cprintf("sensor: sgp30 at 0x%02x read failed: %s (fail count %lu)\n",
                g.addr, g.last_fail ? g.last_fail : "?", (unsigned long)g.fail_count);
        return;
    }
    cprintf("sgp30 at 0x%02x: %u ppm eCO2, %u ppb TVOC (feature set 0x%04x)\n",
            g.addr, r.eco2_ppm, r.tvoc_ppb, r.feature_set);
}

/* --- Sensor Device Contract (Category D) --- */

static bool sgp30_dev_init(struct sensor_dev *dev) {
    if (!sgp30_init()) return false;
    dev->addr = g.addr;
    dev->chan_mask = (1u << SENSOR_CHAN_ECO2) | (1u << SENSOR_CHAN_TVOC);
    return true;
}

static bool sgp30_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    sgp30_reading_t r;
    return sgp30_read(&r);
}

static bool sgp30_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val) {
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

static uint32_t sgp30_dev_selftest(bool report) {
    return sgp30_selftest(report);
}

static const sensor_ops_t sgp30_ops = {
    .init      = sgp30_dev_init,
    .sample    = sgp30_dev_sample,
    .get_value = sgp30_dev_get_value,
    .selftest  = sgp30_dev_selftest,
};

sensor_dev_t sgp30_sensor_dev = {
    .name      = "sgp30",
    .addr      = 0,
    .chan_mask = (1u << SENSOR_CHAN_ECO2) | (1u << SENSOR_CHAN_TVOC),
    .ops       = &sgp30_ops,
    .priv      = NULL,
};
