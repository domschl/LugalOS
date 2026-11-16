#include "drivers/tlv493d.h"
#include "kernel/lock.h"
#include "drivers/i2c_bus.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"

/* Infineon TLV493D-A1B6 driver.
 *
 * The chip has no register addresses: a write is four config bytes, a read
 * returns up to ten bytes from the start of the read map.
 *
 * Read map (bytes 0..6 are the measurement):
 *   0: Bx[11:4]   1: By[11:4]   2: Bz[11:4]
 *   3: Temp[11:8] <<4 | FRM[1:0] <<2 | CH[1:0]   (CH != 0: the frame was not complete)
 *   4: Bx[3:0] <<4 | By[3:0]    5: PD <<4 | Bz[3:0]    6: Temp[7:0]
 *   7..9: factory bits that must be written back unchanged (7: bits 4:3, 8: all, 9: bits 4:0)
 * Write map:
 *   0: reserved, 0
 *   1: P <<7 | IICADDR[1:0] <<5 | factory(7)[4:3] | INT <<2 | FAST <<1 | LOW
 *   2: factory(8)
 *   3: TEMP_DIS <<7 | LP_PERIOD <<6 | PARITY_EN <<5 | factory(9)[4:0]
 * P makes the number of set bits over the four bytes odd; with PARITY_EN set the chip
 * ignores a write that fails the check.
 * FAST=1 LOW=1 is master-controlled mode: a conversion runs when the chip is read.
 * FAST=0 LOW=0 is power-down.
 */

#define RESET_ADDR             0x00u   /* general-call address; the data byte picks the chip's address */
#define RESET_DATA_ADDR_5E     0xFFu   /* SDA high during the reset: the chip comes up at 0x5E */

#define STARTUP_MS             40u
#define RESET_MS               60u
#define CONVERSION_MS          10u

#define LSB_UT                 98      /* 0.098 mT per LSB */

static struct {
    bool               detected;
    uint8_t            addr;
    uint8_t            factory[3];     /* read bytes 7..9, masked */

    tlv493d_reading_t  last;
    uint64_t           last_ms;
    bool               have_last;

    uint32_t           read_count;
    uint32_t           fail_count;
    const char        *last_fail;
} g;   /* guarded by: g_dev_lock (below), as the other sensor drivers */
static ylock_t g_dev_lock;

static bool fail(const char *where) {
    g.last_fail = where;
    g.fail_count++;
    return false;
}

bool        tlv493d_is_detected(void)  { return g.detected; }
uint8_t     tlv493d_address(void)      { return g.addr; }
const char *tlv493d_part_name(void)    { return "tlv493d"; }

uint32_t    tlv493d_read_count(void)   { return g.read_count; }
uint32_t    tlv493d_fail_count(void)   { return g.fail_count; }
const char *tlv493d_last_failure(void) { return g.last_fail; }

bool tlv493d_cached(tlv493d_reading_t *out, uint32_t *age_s) {
    if (!out || !g.have_last) return false;
    *out = g.last;
    if (age_s) {
        uint64_t now = time_get_ms();
        *age_s = (now >= g.last_ms) ? (uint32_t)((now - g.last_ms) / 1000u) : 0u;
    }
    return true;
}

/* 12-bit two's complement, 0.098 mT per LSB. */
int32_t tlv493d_field_ut(int16_t raw12) {
    return (int32_t)raw12 * LSB_UT;
}

/* 25 degC at 340 LSB, 1.1 degC per LSB (datasheet table 'Temperature'). */
int32_t tlv493d_temp_c10(int16_t raw12) {
    return 250 + ((int32_t)raw12 - 340) * 11;
}

void tlv493d_set_parity(uint8_t w[4]) {
    w[1] = (uint8_t)(w[1] | 0x80u);        /* P preset to 1; kept when that already makes the count odd */
    uint8_t y = (uint8_t)(w[0] ^ w[1] ^ w[2] ^ w[3]);
    y ^= (uint8_t)(y >> 4);
    y ^= (uint8_t)(y >> 2);
    y ^= (uint8_t)(y >> 1);
    if ((y & 1u) == 0u) w[1] = (uint8_t)(w[1] & 0x7Fu);
}

static void wait_ms(uint32_t ms) {
    uint64_t until = time_get_ms() + ms;
    while (time_get_ms() < until) sched_yield();
}

static bool write_mode(bool master_controlled) {
    uint8_t w[4];
    w[0] = 0;
    w[1] = (uint8_t)((g.factory[0] & 0x18u) | (master_controlled ? 0x03u : 0x00u));
    w[2] = g.factory[1];
    w[3] = (uint8_t)(0x20u | (master_controlled ? 0x40u : 0x00u) | (g.factory[2] & 0x1Fu));
    tlv493d_set_parity(w);
    return i2c_xfer(g.addr, w, 4u, NULL, 0u);
}

static bool tlv493d_init_locked(void) {
    g.detected = false;
    g.addr = TLV493D_ADDR;

    wait_ms(STARTUP_MS);

    /* The chip relies on an I2C reset after power-up. A controller that cannot address the
     * general-call address leaves the chip in its power-up state, power-down, which the
     * config write below sets again anyway. */
    uint8_t rst = RESET_DATA_ADDR_5E;
    if (i2c_xfer(RESET_ADDR, &rst, 1u, NULL, 0u)) wait_ms(RESET_MS);

    uint8_t rd[10];
    if (!i2c_xfer(g.addr, NULL, 0u, rd, 10u)) return false;

    g.factory[0] = rd[7];
    g.factory[1] = rd[8];
    g.factory[2] = rd[9];
    g.factory[0] &= 0x18u;
    g.factory[2] &= 0x1Fu;

    if (!write_mode(false)) return false;   /* power-down until a sample asks */

    g.detected = true;
    g.have_last = false;
    printk("[TLV493D] TLV493D-A1B6 3D magnetic sensor at 0x%02x.\n", g.addr);
    return true;
}

static int16_t sext12(uint16_t v) {
    return (int16_t)((int16_t)(v << 4) >> 4);
}

static bool tlv493d_read_locked(tlv493d_reading_t *out) {
    if (!out || !g.detected) return false;

    if (!write_mode(true)) return fail("mode write");
    wait_ms(CONVERSION_MS);

    uint8_t b[7];
    bool got = i2c_xfer(g.addr, NULL, 0u, b, 7u);
    bool slept = write_mode(false);

    if (!got) return fail("read");
    if (!slept) return fail("power-down write");
    if (b[3] & 0x03u) return fail("incomplete frame");

    out->bx_ut = tlv493d_field_ut(sext12((uint16_t)((uint16_t)b[0] << 4 | (b[4] >> 4))));
    out->by_ut = tlv493d_field_ut(sext12((uint16_t)((uint16_t)b[1] << 4 | (b[4] & 0x0Fu))));
    out->bz_ut = tlv493d_field_ut(sext12((uint16_t)((uint16_t)b[2] << 4 | (b[5] & 0x0Fu))));
    out->temp_c10 = tlv493d_temp_c10(sext12((uint16_t)((uint16_t)(b[3] >> 4) << 8 | b[6])));

    g.last = *out;
    g.last_ms = time_get_ms();
    g.have_last = true;
    g.read_count++;
    return true;
}

uint32_t tlv493d_selftest(bool report) {
    uint32_t failed = 0;

    /* Datasheet example: 1111 0000 1111 is -241 LSB, -23.6 mT. */
    int32_t f = tlv493d_field_ut(sext12(0xF0Fu));
    if (f != -23618) {
        failed++;
        if (report) cprintf("  tlv493d field: got %ld, want -23618\n", (long)f);
    }
    if (tlv493d_field_ut(sext12(0x7FFu)) != 200606 || tlv493d_field_ut(sext12(0x800u)) != -200704) {
        failed++;
        if (report) cprintf("  tlv493d field range ends wrong\n");
    }
    if (tlv493d_temp_c10(340) != 250 || tlv493d_temp_c10(350) != 360) {
        failed++;
        if (report) cprintf("  tlv493d temperature wrong\n");
    }

    /* Parity: the four bytes carry an odd number of set bits, whatever the payload. */
    static const uint8_t payloads[][4] = {
        { 0, 0x03, 0x00, 0x60 }, { 0, 0x00, 0xFF, 0x7F }, { 0, 0x1B, 0x55, 0x20 }, { 0, 0x00, 0x00, 0x00 },
    };
    for (uint32_t i = 0; i < sizeof(payloads) / sizeof(payloads[0]); i++) {
        uint8_t w[4] = { payloads[i][0], payloads[i][1], payloads[i][2], payloads[i][3] };
        tlv493d_set_parity(w);
        uint32_t ones = 0;
        for (uint32_t k = 0; k < 4u; k++)
            for (uint32_t bit = 0; bit < 8u; bit++) ones += (w[k] >> bit) & 1u;
        if ((ones & 1u) == 0u || (w[1] & 0x7Fu) != payloads[i][1]) {
            failed++;
            if (report) cprintf("  tlv493d parity case %lu failed\n", (unsigned long)i);
        }
    }

    if (report)
        cprintf("tlv493d selftest: %lu case%s failed\n",
                (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}

void tlv493d_print_status(void) {
    if (!g.detected) {
        cprintf("sensor: tlv493d not detected\n");
        return;
    }
    tlv493d_reading_t r;
    if (!tlv493d_read(&r)) {
        cprintf("sensor: tlv493d at 0x%02x read failed: %s (fail count %lu)\n",
                g.addr, g.last_fail ? g.last_fail : "?", (unsigned long)g.fail_count);
        return;
    }
    cprintf("tlv493d at 0x%02x: Bx %ld uT, By %ld uT, Bz %ld uT, die %ld.%ld C\n",
            g.addr, (long)r.bx_ut, (long)r.by_ut, (long)r.bz_ut,
            (long)(r.temp_c10 / 10), (long)((r.temp_c10 < 0 ? -r.temp_c10 : r.temp_c10) % 10));
}

/* --- Sensor Device Contract --- */

#define MAG_MASK ((1u << SENSOR_CHAN_MAG_X) | (1u << SENSOR_CHAN_MAG_Y) | (1u << SENSOR_CHAN_MAG_Z))

static bool tlv493d_dev_init(struct sensor_dev *dev) {
    if (!tlv493d_init()) return false;
    dev->addr = g.addr;
    return true;
}

static bool tlv493d_dev_sample(struct sensor_dev *dev) {
    (void)dev;
    tlv493d_reading_t r;
    return tlv493d_read(&r);
}

static bool tlv493d_dev_get_value(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val) {
    (void)dev;
    if (!g.have_last || !out_val) return false;
    switch (chan) {
    case SENSOR_CHAN_MAG_X: *out_val = g.last.bx_ut; return true;
    case SENSOR_CHAN_MAG_Y: *out_val = g.last.by_ut; return true;
    case SENSOR_CHAN_MAG_Z: *out_val = g.last.bz_ut; return true;
    default: return false;
    }
}

static const sensor_ops_t tlv493d_ops = {
    .init      = tlv493d_dev_init,
    .sample    = tlv493d_dev_sample,
    .get_value = tlv493d_dev_get_value,
    .selftest  = tlv493d_selftest,
};

sensor_dev_t tlv493d_sensor_dev = {
    .name      = "tlv493d",
    .addr      = 0,
    .chan_mask = MAG_MASK,
    .ops       = &tlv493d_ops,
    .priv      = NULL,
};

/* The driver's entry points, under g_dev_lock. */
bool tlv493d_init(void) {
    ylock_acquire(&g_dev_lock);
    bool r = tlv493d_init_locked();
    ylock_release(&g_dev_lock);
    return r;
}

bool tlv493d_read(tlv493d_reading_t *out) {
    ylock_acquire(&g_dev_lock);
    bool r = tlv493d_read_locked(out);
    ylock_release(&g_dev_lock);
    return r;
}
