#include "drivers/bme280.h"
#include "drivers/i2c_rtc.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <string.h>

/* See drivers/include/drivers/bme280.h for the part-vs-part story and why
 * this runs in forced mode. This file is registers and arithmetic.
 *
 * It touches no GPIO and no I2C controller: every access goes through
 * i2c_xfer() (drivers/i2c_rtc.h), which routes to the shared "i2c" task when
 * that is running. So this is ordinary M-mode code with no U-mode fragment in
 * it, and -- unlike every driver in CMakeLists.txt's -fno-jump-tables list --
 * a switch here cannot land a jump table outside a granted region.
 */

#define REG_CALIB_00   0x88u   /* dig_T1..dig_P9, 24 bytes, then 0xA0 unused */
#define REG_ID         0xD0u
#define REG_RESET      0xE0u
#define REG_CALIB_H1   0xA1u
#define REG_CALIB_H2   0xE1u   /* dig_H2..dig_H6, 7 bytes */
#define REG_CTRL_HUM   0xF2u
#define REG_STATUS     0xF3u
#define REG_CTRL_MEAS  0xF4u
#define REG_CONFIG     0xF5u
#define REG_PRESS_MSB  0xF7u   /* 8 bytes through 0xFE on a BME280 */

#define RESET_WORD     0xB6u
#define STATUS_MEASURING (1u << 3)

#define CHIP_ID_BME280 0x60u
#define CHIP_ID_BMP280 0x58u
/* Engineering samples of the BMP280 shipped with these; accepted as BMP280
 * because they are, and refusing them would be refusing a working part. */
#define CHIP_ID_BMP280_SAMPLE_1 0x56u
#define CHIP_ID_BMP280_SAMPLE_2 0x57u

/* Oversampling: the datasheet's own "weather monitoring" preset -- x1 on all
 * three, filter off, forced mode. It is what Bosch recommends for exactly
 * this application (one sample every minute or slower), it keeps a
 * measurement under 10 ms, and a short measurement is what keeps self-heating
 * from biasing the temperature. Higher oversampling buys resolution this
 * application cannot use: pressure at x1 is already ~2.6 Pa RMS, which is
 * 0.026 hPa. */
#define OSRS_X1 0x01u
#define CTRL_MEAS_FORCED ((OSRS_X1 << 5) | (OSRS_X1 << 2) | 0x01u)

static struct {
    bme280_part_t  part;
    uint8_t        addr;
    bme280_calib_t cal;
    /* Which bus transfer failed last, and how many have. Hardware
     * instrumentation: on the first board this ran on, one read in three
     * failed once the radio was up, and "the measurement failed" does not
     * say which of a read's several transfers did. */
    const char    *last_fail;
    uint32_t       fail_count;
    uint32_t       poll_count;
} g;

static bool rd(uint8_t reg, uint8_t *dst, uint32_t len) {
    return i2c_xfer(g.addr, &reg, 1u, dst, len);
}

static bool wr(uint8_t reg, uint8_t value) {
    uint8_t w[2] = { reg, value };
    return i2c_xfer(g.addr, w, 2u, NULL, 0u);
}

static uint16_t u16le(const uint8_t *p) { return (uint16_t)((uint16_t)p[1] << 8 | p[0]); }
static int16_t  s16le(const uint8_t *p) { return (int16_t)u16le(p); }

bme280_part_t bme280_part(void)     { return g.part; }
uint8_t       bme280_address(void)  { return g.addr; }
bool          bme280_is_detected(void) { return g.part != BME280_PART_NONE; }
const bme280_calib_t *bme280_calibration(void) { return &g.cal; }

const char *bme280_part_name(void) {
    switch (g.part) {
        case BME280_PART_BME280: return "bme280";
        case BME280_PART_BMP280: return "bmp280";
        default: return "none";
    }
}

static bme280_part_t part_from_id(uint8_t id) {
    if (id == CHIP_ID_BME280) return BME280_PART_BME280;
    if (id == CHIP_ID_BMP280 || id == CHIP_ID_BMP280_SAMPLE_1 ||
        id == CHIP_ID_BMP280_SAMPLE_2) return BME280_PART_BMP280;
    return BME280_PART_NONE;
}

/* The lookalikes, named so an afternoon is not spent on one. A breakout
 * board sold as "BMP280" may carry any of these, and only the chip id tells
 * them apart. */
static const char *foreign_part(uint8_t id) {
    switch (id) {
        case 0x55u: return "BMP180 (a different register map entirely)";
        case 0x50u: return "BMP388";
        case 0x58u: return NULL;
        case 0x61u: return "BME680";
        default: return NULL;
    }
}

static bool read_calibration(void) {
    uint8_t buf[26];
    if (!rd(REG_CALIB_00, buf, 24u)) return false;

    g.cal.dig_T1 = u16le(buf + 0);
    g.cal.dig_T2 = s16le(buf + 2);
    g.cal.dig_T3 = s16le(buf + 4);
    g.cal.dig_P1 = u16le(buf + 6);
    g.cal.dig_P2 = s16le(buf + 8);
    g.cal.dig_P3 = s16le(buf + 10);
    g.cal.dig_P4 = s16le(buf + 12);
    g.cal.dig_P5 = s16le(buf + 14);
    g.cal.dig_P6 = s16le(buf + 16);
    g.cal.dig_P7 = s16le(buf + 18);
    g.cal.dig_P8 = s16le(buf + 20);
    g.cal.dig_P9 = s16le(buf + 22);

    g.cal.has_humidity = (g.part == BME280_PART_BME280);
    if (!g.cal.has_humidity) return true;

    uint8_t h1 = 0, h[7];
    if (!rd(REG_CALIB_H1, &h1, 1u)) return false;
    if (!rd(REG_CALIB_H2, h, 7u)) return false;
    g.cal.dig_H1 = h1;
    g.cal.dig_H2 = s16le(h + 0);
    g.cal.dig_H3 = h[2];
    /* H4 and H5 are twelve-bit values sharing the nibbles of one byte, which
     * is the single most commonly mis-transcribed line in every BME280
     * driver: H4 takes 0xE5's *low* nibble, H5 its *high* one. */
    g.cal.dig_H4 = (int16_t)(((int16_t)(int8_t)h[3] << 4) | (h[4] & 0x0Fu));
    g.cal.dig_H5 = (int16_t)(((int16_t)(int8_t)h[5] << 4) | (h[4] >> 4));
    g.cal.dig_H6 = (int8_t)h[6];
    return true;
}

bme280_part_t bme280_init(void) {
    static const uint8_t addrs[2] = { BME280_ADDR_LOW, BME280_ADDR_HIGH };

    g.part = BME280_PART_NONE;
    for (uint32_t i = 0; i < 2u; i++) {
        g.addr = addrs[i];
        uint8_t id = 0;
        if (!rd(REG_ID, &id, 1u)) continue;

        bme280_part_t part = part_from_id(id);
        if (part == BME280_PART_NONE) {
            const char *other = foreign_part(id);
            if (other) printk("[BME280] 0x%02x answers with chip id 0x%02x -- that is a %s.\n",
                              g.addr, id, other);
            continue;
        }
        g.part = part;

        /* A reset costs 2 ms and removes "whatever the last boot left in the
         * control registers" from the list of things a wrong reading could
         * be. */
        (void)wr(REG_RESET, RESET_WORD);
        uint64_t until = time_get_ms() + 10u;
        while (time_get_ms() < until) sched_yield();

        if (!read_calibration()) {
            printk("[BME280] Found a %s at 0x%02x but could not read its calibration.\n",
                   bme280_part_name(), g.addr);
            g.part = BME280_PART_NONE;
            continue;
        }

        /* ctrl_hum BEFORE ctrl_meas, and this order is not a preference: the
         * humidity oversampling setting only takes effect on the next write
         * to ctrl_meas. A driver that writes them the other way round reads
         * humidity at the wrong oversampling, or zero, and everything else
         * looks fine. */
        if (g.part == BME280_PART_BME280) (void)wr(REG_CTRL_HUM, OSRS_X1);
        (void)wr(REG_CONFIG, 0x00u);            /* filter off, no standby */
        (void)wr(REG_CTRL_MEAS, CTRL_MEAS_FORCED);

        printk("[BME280] %s at 0x%02x on the shared I2C bus.\n",
               bme280_part_name(), g.addr);
        return g.part;
    }
    g.addr = 0;
    return BME280_PART_NONE;
}

void bme280_compensate(const bme280_calib_t *cal,
                       int32_t raw_temp, int32_t raw_press, int32_t raw_hum,
                       bme280_reading_t *out) {
    if (!cal || !out) return;
    memset(out, 0, sizeof(*out));

    /* --- temperature, 0.01 C, and t_fine which the other two need --- */
    int32_t var1 = (((raw_temp >> 3) - ((int32_t)cal->dig_T1 << 1)) * (int32_t)cal->dig_T2) >> 11;
    int32_t var2 = ((((raw_temp >> 4) - (int32_t)cal->dig_T1) *
                     ((raw_temp >> 4) - (int32_t)cal->dig_T1)) >> 12) * (int32_t)cal->dig_T3 >> 14;
    int32_t t_fine = var1 + var2;
    out->temperature_c100 = (t_fine * 5 + 128) >> 8;

    /* --- pressure, Q24.8 pascals ---
     * The 64-bit formula, not the 32-bit one: net/ntp.c already does signed
     * 64-bit arithmetic on rv32 so the libgcc helpers are proven present, and
     * the 32-bit variant's documented accuracy loss buys nothing. */
    int64_t p1 = (int64_t)t_fine - 128000;
    int64_t p2 = p1 * p1 * (int64_t)cal->dig_P6;
    p2 = p2 + ((p1 * (int64_t)cal->dig_P5) << 17);
    p2 = p2 + ((int64_t)cal->dig_P4 << 35);
    p1 = ((p1 * p1 * (int64_t)cal->dig_P3) >> 8) + ((p1 * (int64_t)cal->dig_P2) << 12);
    p1 = (((int64_t)1 << 47) + p1) * (int64_t)cal->dig_P1 >> 33;
    if (p1 == 0) {
        out->pressure_pa256 = 0;               /* the datasheet's own guard */
    } else {
        int64_t p = 1048576 - raw_press;
        p = (((p << 31) - p2) * 3125) / p1;
        int64_t v1 = ((int64_t)cal->dig_P9 * (p >> 13) * (p >> 13)) >> 25;
        int64_t v2 = ((int64_t)cal->dig_P8 * p) >> 19;
        p = ((p + v1 + v2) >> 8) + ((int64_t)cal->dig_P7 << 4);
        out->pressure_pa256 = (uint32_t)p;
    }

    /* --- humidity, Q22.10 %RH --- */
    if (!cal->has_humidity) return;
    int32_t v = t_fine - 76800;
    v = ((((raw_hum << 14) - ((int32_t)cal->dig_H4 << 20) - ((int32_t)cal->dig_H5 * v)) + 16384) >> 15) *
        (((((((v * (int32_t)cal->dig_H6) >> 10) *
             (((v * (int32_t)cal->dig_H3) >> 11) + 32768)) >> 10) + 2097152) *
          (int32_t)cal->dig_H2 + 8192) >> 14);
    v = v - ((((v >> 15) * (v >> 15)) >> 7) * (int32_t)cal->dig_H1 >> 4);
    if (v < 0) v = 0;
    if (v > 419430400) v = 419430400;
    out->humidity_rh1024 = (uint32_t)(v >> 12);
    out->have_humidity = true;
}

static bool fail(const char *where) {
    g.last_fail = where;
    g.fail_count++;
    return false;
}

bool bme280_read(bme280_reading_t *out) {
    if (!out || g.part == BME280_PART_NONE) return false;

    /* Forced mode is one-shot: the part returns to sleep after each
     * measurement, so every read starts by asking for one. */
    if (g.part == BME280_PART_BME280 && !wr(REG_CTRL_HUM, OSRS_X1)) return fail("ctrl_hum write");
    if (!wr(REG_CTRL_MEAS, CTRL_MEAS_FORCED)) return fail("ctrl_meas write");

    /* Under 10 ms at this oversampling. Polled rather than delayed blindly,
     * and bounded rather than polled forever -- the rule every wait in this
     * tree follows. */
    uint64_t deadline = time_get_ms() + 100u;
    for (;;) {
        uint8_t status = 0;
        g.poll_count++;
        if (!rd(REG_STATUS, &status, 1u)) return fail("status poll");
        if (!(status & STATUS_MEASURING)) break;
        if (time_get_ms() >= deadline) {
            printk("[BME280] The measurement did not finish within 100 ms.\n");
            return fail("measurement timeout");
        }
        sched_yield();
    }

    uint8_t d[8];
    uint32_t want = (g.part == BME280_PART_BME280) ? 8u : 6u;
    if (!rd(REG_PRESS_MSB, d, want)) return fail("data read");

    int32_t raw_press = (int32_t)(((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | (d[2] >> 4));
    int32_t raw_temp  = (int32_t)(((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | (d[5] >> 4));
    int32_t raw_hum   = (want == 8u) ? (int32_t)(((uint32_t)d[6] << 8) | d[7]) : 0;

    bme280_compensate(&g.cal, raw_temp, raw_press, raw_hum, out);
    return true;
}

/* --- The vector ---
 *
 * The calibration block and raw values are tools/bme280_reference.py's
 * VECTOR_CAL/VECTOR_RAW, and the expected results are what that script
 * computes from the datasheet formulas independently of this file. Two
 * transcriptions agreeing shows the arithmetic was not mistyped, which is the
 * failure these formulas actually have. It does not prove the register map --
 * both read the same map from the same page -- which is what the hardware
 * exit criterion and a golden vector from the fitted part are for. */
uint32_t bme280_selftest(bool report) {
    static const bme280_calib_t cal = {
        .dig_T1 = 28455, .dig_T2 = 26619, .dig_T3 = 50,
        .dig_P1 = 37356, .dig_P2 = -10646, .dig_P3 = 3024, .dig_P4 = 6821,
        .dig_P5 = -108, .dig_P6 = -7, .dig_P7 = 9900, .dig_P8 = -10230,
        .dig_P9 = 4285,
        .dig_H1 = 75, .dig_H2 = 366, .dig_H3 = 0,
        .dig_H4 = 306, .dig_H5 = 50, .dig_H6 = 30,
        .has_humidity = true,
    };
    const int32_t  want_t = 2050;
    const uint32_t want_p = 25332935u;
    const uint32_t want_h = 75044u;

    bme280_reading_t r;
    bme280_compensate(&cal, 519888, 343040, 32768, &r);

    uint32_t failed = 0;
    if (r.temperature_c100 != want_t) {
        failed++;
        if (report) cprintf("  temperature: got %ld, want %ld (x0.01 C)\n",
                            (long)r.temperature_c100, (long)want_t);
    }
    if (r.pressure_pa256 != want_p) {
        failed++;
        if (report) cprintf("  pressure: got %lu, want %lu (Q24.8 Pa)\n",
                            (unsigned long)r.pressure_pa256, (unsigned long)want_p);
    }
    if (r.humidity_rh1024 != want_h) {
        failed++;
        if (report) cprintf("  humidity: got %lu, want %lu (Q22.10 %%RH)\n",
                            (unsigned long)r.humidity_rh1024, (unsigned long)want_h);
    }

    /* A BMP280 has no humidity, and must report none rather than zero -- the
     * difference between "dry" and "this part cannot say". */
    bme280_calib_t nohum = cal;
    nohum.has_humidity = false;
    bme280_compensate(&nohum, 519888, 343040, 32768, &r);
    if (r.have_humidity) {
        failed++;
        if (report) cprintf("  a part without humidity reported a humidity reading\n");
    }
    if (r.temperature_c100 != want_t) {
        failed++;
        if (report) cprintf("  temperature changed when humidity was disabled\n");
    }

    if (report) cprintf("bme280 selftest: %lu case%s failed\n",
                        (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}

void bme280_print_status(void) {
    if (g.part == BME280_PART_NONE) {
        cprintf("sensor: none found at 0x76 or 0x77 on the shared I2C bus\n");
        cprintf("        `i2c scan` lists what is actually answering there\n");
        return;
    }

    bme280_reading_t r;
    uint32_t polls_before = g.poll_count;
    if (!bme280_read(&r)) {
        cprintf("sensor: %s at 0x%02x, but the measurement failed -- %s "
                "(failures %lu, polls this read %lu)\n",
                bme280_part_name(), g.addr, g.last_fail ? g.last_fail : "?",
                (unsigned long)g.fail_count,
                (unsigned long)(g.poll_count - polls_before));
        return;
    }
    uint32_t polls = g.poll_count - polls_before;

    /* Printed as fixed-point by hand: there is no float formatting in this
     * kernel's printk, and introducing one for three numbers would be a poor
     * trade. */
    long t_whole = (long)r.temperature_c100 / 100;
    long t_frac  = (long)r.temperature_c100 % 100;
    if (t_frac < 0) t_frac = -t_frac;
    uint32_t pa = r.pressure_pa256 >> 8;

    cprintf("%s @0x%02x: %ld.%02ld C, %lu.%02lu hPa",
            bme280_part_name(), g.addr, t_whole, t_frac,
            (unsigned long)(pa / 100u), (unsigned long)(pa % 100u));
    if (r.have_humidity) {
        uint32_t rh10 = (r.humidity_rh1024 * 10u) >> 10;
        cprintf(", %lu.%lu %%RH", (unsigned long)(rh10 / 10u), (unsigned long)(rh10 % 10u));
    }
    cprintf("  (forced mode, x1 oversampling, %lu status poll%s)\n",
            (unsigned long)polls, polls == 1u ? "" : "s");
}
