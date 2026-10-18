#include "drivers/bmi270.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"
#include "kernel/console.h"
#include "kernel/imath.h"
#include "kernel/lock.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "kernel/sched.h"
#include <stddef.h>
#include <string.h>

extern const uint8_t bmi270_config_file[];
extern const size_t bmi270_config_file_len;

/* Data bytes per INIT_DATA burst: the controller takes 30 bytes per write,
 * one of which is the register; the chip wants whole 16-bit words. */
#define CFG_CHUNK 28u

/* guarded by: g_lock (below) for the configuration state and the last
 * reading; bmi270_init() runs from `sensor init` (see drivers/sensor_hub.c). */
static struct {
    bool             detected;
    bool             configured;    /* microcode loaded, sensors set up */
    uint8_t          load_tries;
    uint8_t          bus, addr;
    bmi270_reading_t last;
    bool             have_last;
} g;

bool bmi270_is_detected(void) { return g.detected; }
bool bmi270_is_configured(void) { return g.configured; }

static ylock_t g_lock;

void bmi270_reload(void) {
    ylock_acquire(&g_lock);
    g.configured = false;
    g.load_tries = 0;
    ylock_release(&g_lock);
}

/* +-4 g: 8192 LSB per g */
int32_t bmi270_acc_mg(int16_t raw) {
    int32_t v = (int32_t)raw * 1000;
    return (v >= 0 ? v + 4096 : v - 4096) / 8192;
}

/* +-1000 deg/s: 32.768 LSB per deg/s, so 0.1 deg/s = raw * 10000 / 32768 */
int32_t bmi270_gyr_ddps(int16_t raw) {
    int32_t v = (int32_t)raw * 10000;
    return (v >= 0 ? v + 16384 : v - 16384) / 32768;
}

static bool wr8(uint8_t reg, uint8_t val) {
    return i2c_reg_write_u8_bus(g.bus, g.addr, reg, val);
}

static bool rd8(uint8_t reg, uint8_t *val) {
    return i2c_reg_read_u8_bus(g.bus, g.addr, reg, val);
}

/* Bosch's sequence (BMI270 datasheet 4.3, SensorAPI bmi270_init): soft
 * reset, advanced power save off, INIT_CTRL 0, the microcode in bursts with
 * INIT_ADDR in words, INIT_CTRL 1, then INTERNAL_STATUS must say 1 within
 * 20 ms (Bosch waits up to 150). */
static const char *bmi270_load_config(void) {
    if (!wr8(BMI270_REG_CMD, 0xB6u)) return "soft reset";
    time_delay_us(2000);
    if (!wr8(BMI270_REG_PWR_CONF, 0x00u)) return "power save off";
    time_delay_us(1000);
    if (!wr8(BMI270_REG_INIT_CTRL, 0x00u)) return "INIT_CTRL 0";
    for (uint32_t i = 0; i < bmi270_config_file_len; i += CFG_CHUNK) {
        uint32_t n = bmi270_config_file_len - i;
        if (n > CFG_CHUNK) n = CFG_CHUNK;
        uint8_t addr[2] = { (uint8_t)((i / 2u) & 0x0Fu), (uint8_t)((i / 2u) >> 4) };
        /* A burst sets its own address first, so a failed one is simply
         * sent again: on the Tab5 the display's bring-up disturbs the bus
         * for a moment about two seconds into boot, which is when the
         * sampler first gets here. */
        bool ok = false;
        for (int t = 0; t < 4 && !ok; t++) {
            ok = i2c_reg_write_bytes_bus(g.bus, g.addr, BMI270_REG_INIT_ADDR0, addr, 2) &&
                 i2c_reg_write_bytes_bus(g.bus, g.addr, BMI270_REG_INIT_DATA, &bmi270_config_file[i], n);
            if (!ok) time_delay_us(5000);
        }
        if (!ok) return "INIT_DATA";
        /* The bus task serves one caller at a time and queues nobody: back
         * to back, these bursts kept the touch poller from ever getting a
         * turn. A millisecond between them is the poller's chance. */
        task_sleep_ms(1);
    }
    if (!wr8(BMI270_REG_INIT_CTRL, 0x01u)) return "INIT_CTRL 1";
    uint8_t st = 0;
    for (int t = 0; t < 15; t++) {
        time_delay_us(10000);
        if (rd8(BMI270_REG_INT_STATUS, &st) && (st & 0x0Fu) == 0x01u) return NULL;
    }
    static char why[32];   /* guarded by: g_lock, as its only caller's printk */
    ksnprintf(why, sizeof why, "INTERNAL_STATUS 0x%02x", (unsigned)st);
    return why;
}

/* The upload is about 300 bursts, close to a second at 100 kHz, so it is
 * not done at probe time (which the boot waits for) but by the first read,
 * on the sampler task. Three tries, then the part is given up. */
static bool bmi270_configure(void) {
    if (g.configured) return true;
    if (g.load_tries >= 3) return false;
    g.load_tries++;
    uint64_t t0 = time_get_ms();
    const char *why = bmi270_load_config();
    if (why) {
        printk("[BMI270] at bus %u 0x%02x: the configuration did not load (try %u): %s\n",
               g.bus, g.addr, (unsigned)g.load_tries, why);
        return false;
    }
    /* acc + gyr + temp on; acc 100 Hz, gyr 200 Hz, normal averaging,
     * performance mode; +-4 g, +-1000 deg/s */
    bool ok = wr8(BMI270_REG_PWR_CTRL, 0x0Eu) && wr8(BMI270_REG_ACC_CONF, 0xA8u) &&
              wr8(BMI270_REG_ACC_RANGE, 0x01u) && wr8(BMI270_REG_GYR_CONF, 0xA9u) &&
              wr8(BMI270_REG_GYR_RANGE, 0x01u);
    if (!ok) return false;
    g.configured = true;
    printk("[BMI270] IMU at bus %u 0x%02x, configuration loaded in %lu ms\n",
           g.bus, g.addr, (unsigned long)(time_get_ms() - t0));
    return true;
}

static bool bmi270_try(uint8_t bus, uint8_t addr) {
    uint8_t id = 0;
    if (!i2c_reg_read_u8_bus(bus, addr, BMI270_REG_CHIP_ID, &id) || id != BMI270_CHIP_ID) return false;
    g.bus = bus;
    g.addr = addr;
    return true;
}

static bool bmi270_init(void) {
    uint8_t buses = i2c_bus_count();
    for (uint8_t b = 0; b < buses; b++) {
        for (uint8_t addr = 0x68u; addr <= 0x69u; addr++) {
            const char *part = i2c_bus_part(b, addr);
            bool ours = part && strcmp(part, "bmi270") == 0;
            if (addr == 0x68u && !ours) continue;
            if (bmi270_try(b, addr)) {
                g.detected = true;
                return true;
            }
        }
    }
    g.detected = false;
    return false;
}

/* The sampler, the orientation task and the shell all read; the first read
 * also uploads the microcode, which must happen once: under g_lock (above). */

bool bmi270_read(bmi270_reading_t *out) {
    if (!g.detected) return false;
    uint8_t d[12];
    ylock_acquire(&g_lock);
    bool ok = bmi270_configure() &&
              i2c_reg_read_bytes_bus(g.bus, g.addr, BMI270_REG_DATA, d, sizeof d);
    bmi270_reading_t r = { 0 };
    if (ok) {
        int16_t v[6];
        for (int i = 0; i < 6; i++) v[i] = (int16_t)((uint16_t)d[2 * i] | ((uint16_t)d[2 * i + 1] << 8));
        r = (bmi270_reading_t){
            bmi270_acc_mg(v[0]), bmi270_acc_mg(v[1]), bmi270_acc_mg(v[2]),
            bmi270_gyr_ddps(v[3]), bmi270_gyr_ddps(v[4]), bmi270_gyr_ddps(v[5]),
        };
        g.last = r;
        g.have_last = true;
    }
    ylock_release(&g_lock);
    if (!ok) return false;
    if (out) *out = r;
    return true;
}

/* Roll about the x axis, pitch about y, from the gravity vector alone. */
void bmi270_tilt(const bmi270_reading_t *r, int32_t *pitch_d10, int32_t *roll_d10) {
    uint64_t yz = (uint64_t)((int64_t)r->ay * r->ay) + (uint64_t)((int64_t)r->az * r->az);
    if (pitch_d10) *pitch_d10 = imath_atan2_deg10(-r->ax, (int32_t)imath_isqrt64(yz));
    if (roll_d10) *roll_d10 = imath_atan2_deg10(r->ay, r->az);
}

uint32_t bmi270_selftest(bool report) {
    uint32_t failed = 0;
    static const struct { int16_t raw; int32_t mg; } acc[] = {
        { 0, 0 }, { 8192, 1000 }, { -8192, -1000 }, { 4, 0 }, { 5, 1 }, { 32767, 4000 }, { -32768, -4000 },
    };
    for (unsigned i = 0; i < sizeof acc / sizeof acc[0]; i++) {
        int32_t got = bmi270_acc_mg(acc[i].raw);
        if (got != acc[i].mg) {
            failed++;
            if (report) cprintf("  bmi270 acc %d -> %ld mg, want %ld\n", acc[i].raw, (long)got, (long)acc[i].mg);
        }
    }
    static const struct { int16_t raw; int32_t d; } gyr[] = {
        { 0, 0 }, { 3277, 1000 }, { -3277, -1000 }, { 32767, 10000 }, { 2, 1 },
    };
    for (unsigned i = 0; i < sizeof gyr / sizeof gyr[0]; i++) {
        int32_t got = bmi270_gyr_ddps(gyr[i].raw);
        if (got != gyr[i].d) {
            failed++;
            if (report) cprintf("  bmi270 gyr %d -> %ld, want %ld\n", gyr[i].raw, (long)got, (long)gyr[i].d);
        }
    }
    /* Flat (z up): pitch 0, roll 0. Tilted 30 deg about y: pitch -30. */
    static const struct { bmi270_reading_t r; int32_t pitch, roll; } tilt[] = {
        { { 0, 0, 1000, 0, 0, 0 }, 0, 0 },
        { { 500, 0, 866, 0, 0, 0 }, -300, 0 },
        { { 0, 707, 707, 0, 0, 0 }, 0, 450 },
        { { 0, 1000, 0, 0, 0, 0 }, 0, 900 },
    };
    for (unsigned i = 0; i < sizeof tilt / sizeof tilt[0]; i++) {
        int32_t p, r;
        bmi270_tilt(&tilt[i].r, &p, &r);
        if (p < tilt[i].pitch - 2 || p > tilt[i].pitch + 2 || r < tilt[i].roll - 2 || r > tilt[i].roll + 2) {
            failed++;
            if (report) cprintf("  bmi270 tilt %u: %ld/%ld, want %ld/%ld\n", i, (long)p, (long)r,
                                (long)tilt[i].pitch, (long)tilt[i].roll);
        }
    }
    failed += imath_selftest(report);
    if (report) cprintf("bmi270 selftest: %lu case%s failed\n", (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}

/* --- Sensor Device-Class Contract --- */

static bool bmi270_dev_init(struct sensor_dev *dev) {
    if (!bmi270_init()) return false;
    dev->bus = g.bus;
    dev->addr = g.addr;
    return true;
}

static bool bmi270_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    return bmi270_read(NULL);
}

static bool bmi270_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out) {
    (void)dev;
    if (!g.have_last || !out) return false;
    switch (chan) {
    case SENSOR_CHAN_ACCEL_X: *out = g.last.ax; return true;
    case SENSOR_CHAN_ACCEL_Y: *out = g.last.ay; return true;
    case SENSOR_CHAN_ACCEL_Z: *out = g.last.az; return true;
    case SENSOR_CHAN_GYRO_X:  *out = g.last.gx; return true;
    case SENSOR_CHAN_GYRO_Y:  *out = g.last.gy; return true;
    case SENSOR_CHAN_GYRO_Z:  *out = g.last.gz; return true;
    default:                  return false;
    }
}

static const sensor_ops_t bmi270_ops = {
    .init      = bmi270_dev_init,
    .sample    = bmi270_dev_sample,
    .get_value = bmi270_dev_get_value,
    .selftest  = bmi270_selftest,
};

/* Once a second in the hub's cache; auto-orientation reads the chip itself
 * at its own rate (bmi270_read). */
sensor_dev_t bmi270_sensor_dev = {
    .name      = "bmi270",
    .chan_mask = (1u << SENSOR_CHAN_ACCEL_X) | (1u << SENSOR_CHAN_ACCEL_Y) | (1u << SENSOR_CHAN_ACCEL_Z) |
                 (1u << SENSOR_CHAN_GYRO_X) | (1u << SENSOR_CHAN_GYRO_Y) | (1u << SENSOR_CHAN_GYRO_Z),
    .period_ms = 1000,
    .ops       = &bmi270_ops,
};
