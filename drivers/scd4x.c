#include "drivers/scd4x.h"
#include "kernel/lock.h"
#include "drivers/i2c_bus.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"

/* Sensirion SCD4x driver (SCD40 / SCD41 / SCD43).
 *
 * 16-bit command protocol, every data word followed by a CRC-8 (polynomial 0x31).
 * Periodic mode: the chip converts every 5 s and holds the newest result until
 * read_measurement drains it, so a reader polling slower than 5 s always finds
 * a fresh sample; get_data_ready_status says whether one is waiting. A command
 * other than read_measurement / get_data_ready_status / set_ambient_pressure
 * is ignored while periodic mode runs, hence the stop (500 ms) before probing.
 */

#define CMD_START_PERIODIC     0x21B1u
#define CMD_READ_MEASUREMENT   0xEC05u
#define CMD_STOP_PERIODIC      0x3F86u
#define CMD_SET_AMBIENT_P      0xE000u
#define CMD_DATA_READY         0xE4B8u
#define CMD_GET_VARIANT        0x202Fu

#define FIRST_SAMPLE_WAIT_MS   6000u

static struct {
    bool             detected;
    uint8_t          addr;
    const char      *part;

    scd4x_reading_t  last;
    uint64_t         last_ms;
    bool             have_last;

    uint32_t         read_count;
    uint32_t         fail_count;
    const char      *last_fail;
} g;   /* guarded by: g_dev_lock (below), as the other sensor drivers */
static ylock_t g_dev_lock;

static bool fail(const char *where) {
    g.last_fail = where;
    g.fail_count++;
    return false;
}

bool        scd4x_is_detected(void)  { return g.detected; }
uint8_t     scd4x_address(void)      { return g.addr; }
const char *scd4x_part_name(void)    { return g.part ? g.part : "scd4x"; }

uint32_t    scd4x_read_count(void)   { return g.read_count; }
uint32_t    scd4x_fail_count(void)   { return g.fail_count; }
const char *scd4x_last_failure(void) { return g.last_fail; }

bool scd4x_cached(scd4x_reading_t *out, uint32_t *age_s) {
    if (!out || !g.have_last) return false;
    *out = g.last;
    if (age_s) {
        uint64_t now = time_get_ms();
        *age_s = (now >= g.last_ms) ? (uint32_t)((now - g.last_ms) / 1000u) : 0u;
    }
    return true;
}

uint8_t scd4x_crc8(const uint8_t *data, uint32_t len) {
    uint8_t crc = 0xFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 8; bit > 0; --bit)
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x31u) : (uint8_t)(crc << 1);
    }
    return crc;
}

/* T = -45 + 175 * raw / 65535 degC; RH = 100 * raw / 65535 %. Rounded to nearest. */
int32_t scd4x_temp_c100(uint16_t raw) {
    return -4500 + (int32_t)(((int64_t)17500 * raw + 32767) / 65535);
}

int32_t scd4x_rh_c100(uint16_t raw) {
    return (int32_t)(((int64_t)10000 * raw + 32767) / 65535);
}

static void wait_ms(uint32_t ms) {
    uint64_t until = time_get_ms() + ms;
    while (time_get_ms() < until) sched_yield();
}

static bool send_cmd(uint16_t cmd) {
    uint8_t b[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFFu) };
    return i2c_xfer(g.addr, b, 2u, NULL, 0u);
}

/* Reads `words` CRC-protected words after a command; execution time wait_ms_cmd. */
static bool read_words(uint16_t cmd, uint32_t wait_ms_cmd, uint16_t *w, uint32_t words) {
    uint8_t buf[9];
    if (words > 3u || !send_cmd(cmd)) return false;
    wait_ms(wait_ms_cmd);
    if (!i2c_xfer(g.addr, NULL, 0u, buf, words * 3u)) return false;
    for (uint32_t i = 0; i < words; i++) {
        if (scd4x_crc8(&buf[i * 3u], 2u) != buf[i * 3u + 2u]) return false;
        w[i] = (uint16_t)((uint16_t)buf[i * 3u] << 8 | buf[i * 3u + 1u]);
    }
    return true;
}

static bool scd4x_init_locked(void) {
    g.detected = false;
    g.addr = SCD4X_ADDR;

    /* A chip left in periodic mode by a previous boot ignores everything below. */
    if (!send_cmd(CMD_STOP_PERIODIC)) return false;
    wait_ms(500u);

    uint16_t v;
    if (!read_words(CMD_GET_VARIANT, 1u, &v, 1u)) return false;
    switch (v >> 12) {
    case 0x0: g.part = "scd40"; break;
    case 0x1: g.part = "scd41"; break;
    case 0x5: g.part = "scd43"; break;
    default:  return false;
    }

    if (!send_cmd(CMD_START_PERIODIC)) return false;

    g.detected = true;
    g.have_last = false;
    printk("[SCD4X] %s CO2 sensor at 0x%02x (variant 0x%04x), periodic mode.\n",
           g.part, g.addr, v);
    return true;
}

static bool scd4x_read_locked(scd4x_reading_t *out) {
    if (!out || !g.detected) return false;

    uint64_t deadline = time_get_ms() + FIRST_SAMPLE_WAIT_MS;
    for (;;) {
        uint16_t st;
        if (!read_words(CMD_DATA_READY, 1u, &st, 1u)) return fail("data ready status");
        if (st & 0x07FFu) break;
        if (g.have_last || time_get_ms() >= deadline) return fail("no new measurement");
        wait_ms(250u);
    }

    uint16_t w[3];
    if (!read_words(CMD_READ_MEASUREMENT, 1u, w, 3u)) return fail("read measurement");

    out->co2_ppm   = w[0];
    out->temp_c100 = scd4x_temp_c100(w[1]);
    out->rh_c100   = scd4x_rh_c100(w[2]);

    g.last = *out;
    g.last_ms = time_get_ms();
    g.have_last = true;
    g.read_count++;
    return true;
}

static bool scd4x_set_ambient_pressure_locked(int32_t pa) {
    if (!g.detected || pa < 70000 || pa > 120000) return false;
    uint16_t hpa = (uint16_t)((pa + 50) / 100);
    uint8_t cmd[5] = { (uint8_t)(CMD_SET_AMBIENT_P >> 8), (uint8_t)(CMD_SET_AMBIENT_P & 0xFFu),
                       (uint8_t)(hpa >> 8), (uint8_t)(hpa & 0xFFu), 0 };
    cmd[4] = scd4x_crc8(&cmd[2], 2u);
    return i2c_xfer(g.addr, cmd, 5u, NULL, 0u);
}

uint32_t scd4x_selftest(bool report) {
    uint32_t failed = 0;

    /* Datasheet CRC vectors (Sensirion CRC-8; read_measurement example, set_ambient_pressure). */
    static const struct { uint16_t w; uint8_t crc; } crcs[] = {
        { 0xBEEFu, 0x92u }, { 0x01F4u, 0x33u }, { 0x6667u, 0xA2u },
        { 0x5EB9u, 0x3Cu }, { 0x03DBu, 0x42u },
    };
    for (uint32_t i = 0; i < sizeof(crcs) / sizeof(crcs[0]); i++) {
        uint8_t b[2] = { (uint8_t)(crcs[i].w >> 8), (uint8_t)(crcs[i].w & 0xFFu) };
        uint8_t c = scd4x_crc8(b, 2u);
        if (c != crcs[i].crc) {
            failed++;
            if (report) cprintf("  scd4x crc 0x%04x: got 0x%02x, want 0x%02x\n", crcs[i].w, c, crcs[i].crc);
        }
    }

    /* The datasheet's read_measurement example: 25 degC and 37 %RH. */
    int32_t t = scd4x_temp_c100(0x6667u), rh = scd4x_rh_c100(0x5EB9u);
    if (t != 2500) {
        failed++;
        if (report) cprintf("  scd4x temp: got %ld, want 2500\n", (long)t);
    }
    if (rh != 3700) {
        failed++;
        if (report) cprintf("  scd4x rh: got %ld, want 3700\n", (long)rh);
    }
    if (scd4x_temp_c100(0) != -4500 || scd4x_temp_c100(0xFFFFu) != 13000) {
        failed++;
        if (report) cprintf("  scd4x temp range ends wrong\n");
    }

    if (report)
        cprintf("scd4x selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}

void scd4x_print_status(void) {
    if (!g.detected) {
        cprintf("sensor: scd4x not detected\n");
        return;
    }
    scd4x_reading_t r;
    if (!scd4x_read(&r)) {
        cprintf("sensor: %s at 0x%02x read failed: %s (fail count %lu)\n",
                g.part, g.addr, g.last_fail ? g.last_fail : "?", (unsigned long)g.fail_count);
        return;
    }
    cprintf("%s at 0x%02x: %u ppm CO2, %ld.%02ld C, %ld.%02ld %%RH\n",
            g.part, g.addr, r.co2_ppm,
            (long)(r.temp_c100 / 100), (long)((r.temp_c100 < 0 ? -r.temp_c100 : r.temp_c100) % 100),
            (long)(r.rh_c100 / 100), (long)(r.rh_c100 % 100));
}

/* --- Sensor Device Contract --- */

static bool scd4x_dev_init(struct sensor_dev *dev) {
    if (!scd4x_init()) return false;
    dev->addr = g.addr;
    return true;
}

static bool scd4x_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    scd4x_reading_t r;
    return scd4x_read(&r);
}

static bool scd4x_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val) {
    (void)dev;
    if (!g.have_last || !out_val || chan != SENSOR_CHAN_CO2) return false;
    *out_val = (int32_t)g.last.co2_ppm;
    return true;
}

static const sensor_ops_t scd4x_ops = {
    .init      = scd4x_dev_init,
    .sample    = scd4x_dev_sample,
    .get_value = scd4x_dev_get_value,
    .selftest  = scd4x_selftest,
};

sensor_dev_t scd4x_sensor_dev = {
    .name      = "scd4x",
    .addr      = 0,
    .chan_mask = (1u << SENSOR_CHAN_CO2),
    .ops       = &scd4x_ops,
    .priv      = NULL,
};

/* The driver's entry points, under g_dev_lock. */
bool scd4x_init(void) {
    ylock_acquire(&g_dev_lock);
    bool r = scd4x_init_locked();
    ylock_release(&g_dev_lock);
    return r;
}

bool scd4x_read(scd4x_reading_t *out) {
    ylock_acquire(&g_dev_lock);
    bool r = scd4x_read_locked(out);
    ylock_release(&g_dev_lock);
    return r;
}

bool scd4x_set_ambient_pressure(int32_t pa) {
    ylock_acquire(&g_dev_lock);
    bool r = scd4x_set_ambient_pressure_locked(pa);
    ylock_release(&g_dev_lock);
    return r;
}
