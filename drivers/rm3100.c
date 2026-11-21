#include "drivers/rm3100.h"
#include "kernel/lock.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "kernel/sched.h"

/* PNI RM3100 driver, I2C mode, polling (single measurement) operation.
 *
 * Register addresses are the plain 7-bit numbers; the 0x80 "read" bit of the
 * manual's table belongs to SPI. POLL (0x00) takes the axes to measure in
 * bits 6:4 and the chip idles between measurements, so nothing is running
 * between samples. STATUS bit 7 is DRDY. Results are 24-bit two's complement,
 * big-endian, X then Y then Z.
 *
 * A POLL write while a measurement runs is NACKed, and so is a results read
 * before DRDY; the loop below only reads after it saw DRDY.
 */

#define POLL_XYZ          0x70u
#define STATUS_DRDY       0x80u
#define MEASURE_TIMEOUT_US 60000u   /* 3 axes at 400 cycle counts take ~13 ms */

/* guarded by: g_dev_lock (below), which init and every measurement take:
 * the hub's sampler, `sensor` and Lisp may all measure, and `sensor init`
 * probes again under them. */
static struct {
    bool             detected;
    uint8_t          addr;
    uint8_t          bus;
    uint8_t          revid;
    rm3100_reading_t last;
    bool             have_last;
    uint32_t         read_count;
    uint32_t         fail_count;
    const char      *last_fail;
} g;
static ylock_t g_dev_lock;

static bool fail(const char *where) {
    g.last_fail = where;
    g.fail_count++;
    return false;
}

bool    rm3100_is_detected(void) { return g.detected; }
uint8_t rm3100_address(void)     { return g.addr; }
uint8_t rm3100_bus(void)         { return g.bus; }
uint8_t rm3100_revision(void)    { return g.revid; }

int32_t rm3100_unpack(const uint8_t b[3]) {
    uint32_t v = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
    return (int32_t)(v << 8) >> 8;
}

/* round(raw_sum / n / gain), gain in LSB/uT = (3671 * cc + 15000) / 10000, so
 * nT = raw_sum * 1000 * 10000 / (n * (3671 * cc + 15000)). The numerator
 * reaches 8e6 * 8 * 1e7, which needs 64 bits; half is added away from zero. */
int32_t rm3100_field_nt(int32_t raw_sum, uint32_t n, uint32_t cycle_count) {
    int64_t den = (int64_t)n * (3671 * (int64_t)cycle_count + 15000);
    int64_t num = (int64_t)raw_sum * 10000000;
    num += num < 0 ? -(den / 2) : den / 2;
    return (int32_t)(num / den);
}

static bool rm3100_probe(uint8_t b, uint8_t addr) {
    uint8_t rev = 0;
    if (!i2c_reg_read_u8_bus(b, addr, RM3100_REG_REVID, &rev)) return false;
    if (rev == 0x00u || rev == 0xFFu) return false;

    /* The cycle counts are the chip's own registers, which an ACKing
     * stranger does not keep: set them and read them back. */
    const uint8_t cc[6] = {
        RM3100_CYCLE_COUNT >> 8, RM3100_CYCLE_COUNT & 0xFFu,
        RM3100_CYCLE_COUNT >> 8, RM3100_CYCLE_COUNT & 0xFFu,
        RM3100_CYCLE_COUNT >> 8, RM3100_CYCLE_COUNT & 0xFFu,
    };
    uint8_t back[6] = { 0 };
    if (!i2c_reg_write_bytes_bus(b, addr, RM3100_REG_CCX, cc, sizeof cc)) return false;
    if (!i2c_reg_read_bytes_bus(b, addr, RM3100_REG_CCX, back, sizeof back)) return false;
    for (uint32_t i = 0; i < sizeof cc; i++) if (back[i] != cc[i]) return false;

    g.detected = true;
    g.addr = addr;
    g.bus = b;
    g.revid = rev;
    return true;
}

static bool rm3100_init_locked(void) {
    uint8_t buses = i2c_bus_count();

    g.detected = false;
    g.have_last = false;
    for (uint8_t b = 0; b < buses; b++) {
        for (uint8_t i = 0; i < RM3100_ADDR_COUNT; i++) {
            uint8_t addr = (uint8_t)(RM3100_ADDR_BASE + i);
            if (!rm3100_probe(b, addr)) continue;
            printk("[RM3100] RM3100 3-axis magnetometer at bus %u 0x%02x (rev 0x%02x), %u cycle counts.\n",
                   b, addr, (unsigned)g.revid, (unsigned)RM3100_CYCLE_COUNT);
            return true;
        }
    }
    g.addr = 0;
    return false;
}

/* One measurement of all three axes into raw[]. */
static bool measure(int32_t raw[3]) {
    if (!i2c_reg_write_u8_bus(g.bus, g.addr, RM3100_REG_POLL, POLL_XYZ)) return fail("poll write");

    /* The clock is read before the condition, so a task descheduled past the
     * deadline still gets one look at DRDY. */
    uint64_t deadline = time_get_us() + MEASURE_TIMEOUT_US;
    for (;;) {
        bool late = time_get_us() >= deadline;
        uint8_t st = 0;
        if (i2c_reg_read_u8_bus(g.bus, g.addr, RM3100_REG_STATUS, &st) && (st & STATUS_DRDY)) break;
        if (late) return fail("measurement timeout");
        task_sleep_ms(1);
    }

    uint8_t b[9];
    if (!i2c_reg_read_bytes_bus(g.bus, g.addr, RM3100_REG_MX, b, sizeof b)) return fail("read results");
    for (uint32_t a = 0; a < 3; a++) raw[a] = rm3100_unpack(&b[3u * a]);
    return true;
}

static bool rm3100_read_locked(rm3100_reading_t *out) {
    if (!out || !g.detected) return false;

    int32_t sum[3] = { 0, 0, 0 };
    for (uint32_t k = 0; k < RM3100_AVG; k++) {
        int32_t raw[3];
        if (!measure(raw)) return false;
        for (uint32_t a = 0; a < 3; a++) sum[a] += raw[a];
    }

    out->bx_nt = rm3100_field_nt(sum[0], RM3100_AVG, RM3100_CYCLE_COUNT);
    out->by_nt = rm3100_field_nt(sum[1], RM3100_AVG, RM3100_CYCLE_COUNT);
    out->bz_nt = rm3100_field_nt(sum[2], RM3100_AVG, RM3100_CYCLE_COUNT);

    g.last = *out;
    g.have_last = true;
    g.read_count++;
    return true;
}

uint32_t rm3100_selftest(bool report) {
    uint32_t failed = 0;

    /* Sign extension of the 24-bit result. */
    static const struct { uint8_t b[3]; int32_t want; } unpack[] = {
        { { 0x00, 0x00, 0x00 },        0 },
        { { 0x00, 0x00, 0x01 },        1 },
        { { 0x7F, 0xFF, 0xFF },  8388607 },
        { { 0x80, 0x00, 0x00 }, -8388608 },
        { { 0xFF, 0xFF, 0xFF },       -1 },
        { { 0xFF, 0x12, 0x34 },  -60876 },
    };
    for (uint32_t i = 0; i < sizeof(unpack) / sizeof(unpack[0]); i++) {
        int32_t got = rm3100_unpack(unpack[i].b);
        if (got != unpack[i].want) {
            failed++;
            if (report) cprintf("  rm3100 unpack %lu: got %ld, want %ld\n",
                                (unsigned long)i, (long)got, (long)unpack[i].want);
        }
    }

    /* Gain against the manual's Table 3-1: 20, 38, 75 LSB/uT at 50, 100, 200 cycle
     * counts, i.e. a field of 1 uT is that many counts to within the table's rounding. */
    static const struct { uint32_t cc; int32_t raw; int32_t want_nt; } conv[] = {
        { 200,   75,     1001 },   /* 74.92 LSB/uT */
        { 200,  -75,    -1001 },
        { 100,   38,      995 },   /* 38.21 LSB/uT */
        {  50,   20,     1007 },   /* 19.86 LSB/uT */
        { 400, 7417,    50000 },   /* 148.34 LSB/uT: 50 uT, earth's field */
        { 400,    0,        0 },
        { 400, -7417,  -50000 },
    };
    for (uint32_t i = 0; i < sizeof(conv) / sizeof(conv[0]); i++) {
        int32_t got = rm3100_field_nt(conv[i].raw, 1u, conv[i].cc);
        if (got != conv[i].want_nt) {
            failed++;
            if (report) cprintf("  rm3100 field cc=%lu raw=%ld: got %ld nT, want %ld\n",
                                (unsigned long)conv[i].cc, (long)conv[i].raw, (long)got, (long)conv[i].want_nt);
        }
    }

    /* The mean over n: eight identical results convert as one. */
    if (rm3100_field_nt(8 * 7417, 8u, 400u) != 50000) {
        failed++;
        if (report) cprintf("  rm3100 mean of 8 wrong\n");
    }
    /* Largest sum the driver can form: 8 results at full scale, no overflow. */
    if (rm3100_field_nt(8 * 8388607, 8u, 400u) <= 0) {
        failed++;
        if (report) cprintf("  rm3100 full-scale sum overflowed\n");
    }

    if (report)
        cprintf("rm3100 selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}

/* --- Sensor Device Contract --- */

#define MAG_MASK ((1u << SENSOR_CHAN_MAG_X) | (1u << SENSOR_CHAN_MAG_Y) | (1u << SENSOR_CHAN_MAG_Z))

/* Fast enough to see pulsations, slow enough to leave the hub lock free:
 * a sample holds it for about 8 x 13 ms. */
#define SAMPLE_PERIOD_MS 1000u

static bool rm3100_dev_init(struct sensor_dev *dev) {
    if (!rm3100_init()) return false;
    dev->addr = g.addr;
    dev->bus = g.bus;
    return true;
}

static bool rm3100_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    rm3100_reading_t r;
    return rm3100_read(&r);
}

static bool rm3100_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val) {
    (void)dev;
    if (!g.have_last || !out_val) return false;
    switch (chan) {
    case SENSOR_CHAN_MAG_X: *out_val = g.last.bx_nt; return true;
    case SENSOR_CHAN_MAG_Y: *out_val = g.last.by_nt; return true;
    case SENSOR_CHAN_MAG_Z: *out_val = g.last.bz_nt; return true;
    default: return false;
    }
}

static const sensor_ops_t rm3100_ops = {
    .init      = rm3100_dev_init,
    .sample    = rm3100_dev_sample,
    .get_value = rm3100_dev_get_value,
    .selftest  = rm3100_selftest,
};

sensor_dev_t rm3100_sensor_dev = {
    .name      = "rm3100",
    .addr      = 0,
    .bus       = 0,
    .chan_mask = MAG_MASK,
    .period_ms = SAMPLE_PERIOD_MS,
    .ops       = &rm3100_ops,
    .priv      = NULL,
};

/* The driver's entry points, under g_dev_lock. */
bool rm3100_init(void) {
    ylock_acquire(&g_dev_lock);
    bool r = rm3100_init_locked();
    ylock_release(&g_dev_lock);
    return r;
}

bool rm3100_read(rm3100_reading_t *out) {
    ylock_acquire(&g_dev_lock);
    bool r = rm3100_read_locked(out);
    ylock_release(&g_dev_lock);
    return r;
}
