#include "drivers/mmc5983.h"
#include "kernel/lock.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "kernel/sched.h"

/* MEMSIC MMC5983MA driver.
 *
 * The control registers (0x09..0x0C) are write-only and each bit of CTRL0 is a
 * one-shot command, so every write below is a whole register and nothing is
 * read back. The status register's done bits are cleared by writing 1 to them,
 * which is done before each measurement so a stale bit from the one before is
 * never taken for the new one.
 *
 * CTRL0: TM_M 0x01 (measure field), TM_T 0x02 (measure temperature), SET 0x08, RESET 0x10.
 * CTRL1: SW_RST 0x80; BW[1:0] = 0 is the 8 ms filter, 0.4 mG RMS noise, the quietest.
 * STATUS: Meas_M_Done 0x01, Meas_T_Done 0x02.
 */

#define CTRL0_TM_M         0x01u
#define CTRL0_TM_T         0x02u
#define CTRL0_SET          0x08u
#define CTRL0_RESET        0x10u
#define CTRL1_SW_RST       0x80u
#define STATUS_MEAS_M      0x01u
#define STATUS_MEAS_T      0x02u

#define RESET_MS           15u     /* power-on time after SW_RST is 10 ms */
#define COIL_SETTLE_MS     1u      /* after a SET or RESET pulse (500 ns) before measuring */
#define MEASURE_TIMEOUT_US 40000u  /* the 8 ms filter, with room for a busy bus */

/* guarded by: g_dev_lock (below), which init and every measurement take:
 * the hub's sampler, `sensor` and Lisp may all measure, and `sensor init`
 * probes again under them. */
static struct {
    bool              detected;
    uint8_t           addr;
    uint8_t           bus;
    mmc5983_reading_t last;
    bool              have_last;
    uint32_t          read_count;
    uint32_t          fail_count;
    const char       *last_fail;
} g;
static ylock_t g_dev_lock;

static bool fail(const char *where) {
    g.last_fail = where;
    g.fail_count++;
    return false;
}

bool    mmc5983_is_detected(void) { return g.detected; }
uint8_t mmc5983_address(void)     { return g.addr; }
uint8_t mmc5983_bus(void)         { return g.bus; }

void mmc5983_unpack(const uint8_t b[7], uint32_t raw[3]) {
    raw[0] = ((uint32_t)b[0] << 10) | ((uint32_t)b[1] << 2) | ((b[6] >> 6) & 3u);
    raw[1] = ((uint32_t)b[2] << 10) | ((uint32_t)b[3] << 2) | ((b[6] >> 4) & 3u);
    raw[2] = ((uint32_t)b[4] << 10) | ((uint32_t)b[5] << 2) | ((b[6] >> 2) & 3u);
}

/* diff_sum / pairs / 2 counts, times 100000 nT / 16384 counts = 3125 / 512.
 * The product needs 64 bits for a few pairs at full scale; half is added away
 * from zero before the shift (an arithmetic one, which GCC guarantees). */
int32_t mmc5983_field_nt(int32_t diff_sum, uint32_t pairs_log2) {
    uint32_t shift = 10u + pairs_log2;
    int64_t num = (int64_t)diff_sum * 3125;
    int64_t half = (int64_t)1 << (shift - 1u);
    if (num >= 0) return (int32_t)((num + half) >> shift);
    return -(int32_t)(((-num) + half) >> shift);
}

int32_t mmc5983_temp_c10(uint8_t raw) {
    return (int32_t)raw * 2000 / 255 - 750;
}

/* One measurement, command `cmd` (TM_M or TM_T), waiting for its done bit. */
static bool run(uint8_t cmd, uint8_t done) {
    if (!i2c_reg_write_u8_bus(g.bus, g.addr, MMC5983_REG_STATUS, done)) return false;
    if (!i2c_reg_write_u8_bus(g.bus, g.addr, MMC5983_REG_CTRL0, cmd)) return false;

    /* The clock is read before the condition, so a task descheduled past the
     * deadline still gets one look at the done bit. */
    uint64_t deadline = time_get_us() + MEASURE_TIMEOUT_US;
    for (;;) {
        bool late = time_get_us() >= deadline;
        uint8_t st = 0;
        if (i2c_reg_read_u8_bus(g.bus, g.addr, MMC5983_REG_STATUS, &st) && (st & done)) return true;
        if (late) return false;
        task_sleep_ms(1);
    }
}

static bool coil(uint8_t pulse) {
    if (!i2c_reg_write_u8_bus(g.bus, g.addr, MMC5983_REG_CTRL0, pulse)) return false;
    task_sleep_ms(COIL_SETTLE_MS);
    return true;
}

static bool field(uint32_t raw[3]) {
    uint8_t b[7];
    if (!run(CTRL0_TM_M, STATUS_MEAS_M)) return fail("field measurement timeout");
    if (!i2c_reg_read_bytes_bus(g.bus, g.addr, MMC5983_REG_XOUT0, b, sizeof b)) return fail("read field");
    mmc5983_unpack(b, raw);
    return true;
}

static bool mmc5983_init_locked(void) {
    uint8_t buses = i2c_bus_count();

    g.detected = false;
    g.have_last = false;
    for (uint8_t b = 0; b < buses; b++) {
        uint8_t id = 0;
        if (!i2c_reg_read_u8_bus(b, MMC5983_ADDR, MMC5983_REG_ID, &id)) continue;
        if (id != MMC5983_PRODUCT_ID) continue;

        g.addr = MMC5983_ADDR;
        g.bus = b;
        /* Reset, then the default of everything: 8 ms filter, no continuous mode, no auto SET/RESET. */
        if (!i2c_reg_write_u8_bus(b, MMC5983_ADDR, MMC5983_REG_CTRL1, CTRL1_SW_RST)) return false;
        task_sleep_ms(RESET_MS);
        if (!i2c_reg_write_u8_bus(b, MMC5983_ADDR, MMC5983_REG_CTRL1, 0x00u)) return false;
        if (!i2c_reg_write_u8_bus(b, MMC5983_ADDR, MMC5983_REG_CTRL2, 0x00u)) return false;

        g.detected = true;
        printk("[MMC5983] MMC5983MA 3-axis magnetometer at bus %u 0x%02x, %u SET/RESET pairs per sample.\n",
               b, MMC5983_ADDR, 1u << MMC5983_PAIRS_LOG2);
        return true;
    }
    g.addr = 0;
    return false;
}

static bool mmc5983_read_locked(mmc5983_reading_t *out) {
    if (!out || !g.detected) return false;

    int32_t diff[3] = { 0, 0, 0 };
    for (uint32_t k = 0; k < (1u << MMC5983_PAIRS_LOG2); k++) {
        uint32_t set[3], reset[3];
        if (!coil(CTRL0_SET)) return fail("set write");
        if (!field(set)) return false;
        if (!coil(CTRL0_RESET)) return fail("reset write");
        if (!field(reset)) return false;
        for (uint32_t a = 0; a < 3; a++) diff[a] += (int32_t)set[a] - (int32_t)reset[a];
    }

    /* The die temperature is for diagnosis only; its failure costs no field. */
    uint8_t traw = 0;
    bool have_t = run(CTRL0_TM_T, STATUS_MEAS_T) &&
                  i2c_reg_read_u8_bus(g.bus, g.addr, MMC5983_REG_TOUT, &traw);

    out->bx_nt = mmc5983_field_nt(diff[0], MMC5983_PAIRS_LOG2);
    out->by_nt = mmc5983_field_nt(diff[1], MMC5983_PAIRS_LOG2);
    out->bz_nt = mmc5983_field_nt(diff[2], MMC5983_PAIRS_LOG2);
    out->temp_c10 = have_t ? mmc5983_temp_c10(traw) : g.last.temp_c10;

    g.last = *out;
    g.have_last = true;
    g.read_count++;
    return true;
}

uint32_t mmc5983_selftest(bool report) {
    uint32_t failed = 0;

    /* Bit placement: X, Y, Z take the top two bit pairs of byte 6 in that order. */
    static const uint8_t bytes[7] = { 0x80, 0x01, 0x40, 0x02, 0x20, 0x03, 0xE4 };
    uint32_t raw[3];
    mmc5983_unpack(bytes, raw);
    /* X = 0x80<<10 | 0x01<<2 | 3, Y = 0x40<<10 | 0x02<<2 | 2, Z = 0x20<<10 | 0x03<<2 | 1 */
    if (raw[0] != 0x20007u || raw[1] != 0x1000Au || raw[2] != 0x800Du) {
        failed++;
        if (report) cprintf("  mmc5983 unpack: %lx %lx %lx\n",
                            (unsigned long)raw[0], (unsigned long)raw[1], (unsigned long)raw[2]);
    }

    /* 16384 counts is 1 G (100 uT). A set count 16384 above the reset count is
     * twice a field of 8192 counts, 0.5 G, 50 uT. */
    static const struct { int32_t diff; uint32_t log2; int32_t want_nt; } conv[] = {
        {      0, 0,       0 },
        {  16384, 0,   50000 },   /* 50 uT, earth's field */
        { -16384, 0,  -50000 },
        {  65536, 2,   50000 },   /* four pairs of the same */
        {      1, 0,       3 },   /* 3.05 nT */
        {     -1, 0,      -3 },
        { 262143, 0,  799997 },   /* full scale, 8 G */
        { 4 * 262143, 2, 799997 },
    };
    for (uint32_t i = 0; i < sizeof(conv) / sizeof(conv[0]); i++) {
        int32_t got = mmc5983_field_nt(conv[i].diff, conv[i].log2);
        if (got != conv[i].want_nt) {
            failed++;
            if (report) cprintf("  mmc5983 field diff=%ld/2^%lu: got %ld nT, want %ld\n",
                                (long)conv[i].diff, (unsigned long)conv[i].log2, (long)got, (long)conv[i].want_nt);
        }
    }

    if (mmc5983_temp_c10(0) != -750 || mmc5983_temp_c10(255) != 1250 || mmc5983_temp_c10(128) != 253) {
        failed++;
        if (report) cprintf("  mmc5983 temperature: %ld %ld %ld\n", (long)mmc5983_temp_c10(0),
                            (long)mmc5983_temp_c10(255), (long)mmc5983_temp_c10(128));
    }

    if (report)
        cprintf("mmc5983 selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}

/* --- Sensor Device Contract --- */

#define MAG_MASK ((1u << SENSOR_CHAN_MAG_X) | (1u << SENSOR_CHAN_MAG_Y) | (1u << SENSOR_CHAN_MAG_Z))

/* A sample holds the hub lock for 4 x 2 x ~10 ms. */
#define SAMPLE_PERIOD_MS 1000u

static bool mmc5983_dev_init(struct sensor_dev *dev) {
    if (!mmc5983_init()) return false;
    dev->addr = g.addr;
    dev->bus = g.bus;
    return true;
}

static bool mmc5983_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    mmc5983_reading_t r;
    return mmc5983_read(&r);
}

static bool mmc5983_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val) {
    (void)dev;
    if (!g.have_last || !out_val) return false;
    switch (chan) {
    case SENSOR_CHAN_MAG_X: *out_val = g.last.bx_nt; return true;
    case SENSOR_CHAN_MAG_Y: *out_val = g.last.by_nt; return true;
    case SENSOR_CHAN_MAG_Z: *out_val = g.last.bz_nt; return true;
    default: return false;
    }
}

static const sensor_ops_t mmc5983_ops = {
    .init      = mmc5983_dev_init,
    .sample    = mmc5983_dev_sample,
    .get_value = mmc5983_dev_get_value,
    .selftest  = mmc5983_selftest,
};

sensor_dev_t mmc5983_sensor_dev = {
    .name      = "mmc5983",
    .addr      = 0,
    .bus       = 0,
    .chan_mask = MAG_MASK,
    .period_ms = SAMPLE_PERIOD_MS,
    .ops       = &mmc5983_ops,
    .priv      = NULL,
};

/* The driver's entry points, under g_dev_lock. */
bool mmc5983_init(void) {
    ylock_acquire(&g_dev_lock);
    bool r = mmc5983_init_locked();
    ylock_release(&g_dev_lock);
    return r;
}

bool mmc5983_read(mmc5983_reading_t *out) {
    ylock_acquire(&g_dev_lock);
    bool r = mmc5983_read_locked(out);
    ylock_release(&g_dev_lock);
    return r;
}
