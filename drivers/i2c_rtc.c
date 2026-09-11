#include "drivers/i2c_rtc.h"
#include "drivers/i2c_bus.h"
#include "drivers/at24c32.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "kernel/sched.h"
#include "kernel/chan.h"
#include "kernel/mem_domain.h"
#include "kernel/device.h"
#include "kernel/ipc.h"
#include "kernel/palloc.h"
#include "arch/umode.h"
#include "drivers/uart.h"
#include "lugalos_config.h"
#include <string.h>

#define DS1307_DS3231_I2C_ADDR 0x68
static bool g_rtc_detected = false;

static inline uint8_t bcd2dec(uint8_t val) {
    return ((val >> 4) * 10) + (val & 0x0F);
}

static inline uint8_t dec2bcd(uint8_t val) {
    return ((val / 10) << 4) | (val % 10);
}

/* Forward declarations: direct-hardware access, defined further down this
 * file. Only i2c_rtc_init() (once, at boot, before the task exists) and
 * i2c_task_body() (below) may call these -- every other caller goes through
 * the public i2c_rtc_read_time()/write_time()/read_temperature_c() facades,
 * which route via the shared "i2c" task when it is alive. */
static bool rtc_rd(uint8_t reg, uint8_t *dst, uint32_t len);
static bool rtc_wr(const uint8_t *buf, uint32_t len);
static bool i2c_rtc_hw_read_time(rtc_time_t *tm);
static bool i2c_rtc_hw_write_time(const rtc_time_t *tm);
static bool i2c_rtc_hw_read_temperature_c(int *temp_c);

/* DS3231 status register, and the one bit in it that matters here.
 *
 * OSF is set by the chip whenever its oscillator has stopped -- which is what
 * happens when it loses power with no working backup cell -- and stays set
 * until something clears it. It is the chip saying, in the only way it can,
 * "the time in my registers is meaningless".
 *
 * It has to be read, because the meaningless value is *plausible*: a
 * DS3231 that has lost power reads 2000-01-01 00:00:00, and every range check
 * anyone would write -- month 1-12, day 1-31, hour under 24 -- passes it. A
 * clock face then shows midnight on New Year's Day 2000 with no indication
 * that anything is wrong, which is exactly what a Pico-Clock-Green did for a
 * day while the board's own kernel clock was correct to the millisecond.
 *
 * The DS1307 uses bit 7 of register 0 (CH, "clock halt") for the same job.
 * Both are checked; on a part that has neither, the register reads as
 * something without those bits set and nothing is lost. */
#define DS3231_REG_STATUS 0x0F
#define DS3231_STATUS_OSF 0x80
#define DS3231_REG_TEMP   0x11

/* Which of the two parts is on the bus, decided once at init.
 *
 * They share an address and a time-register layout, which is why one driver
 * has always served both -- but **the registers this file reaches beyond the
 * clock are not the same registers on the two parts**, and treating them as
 * equal is actively harmful rather than merely inaccurate:
 *
 *   0x0F : DS3231 status (OSF). On a DS1307 this is **user NVRAM** -- the
 *          part has 56 battery-backed bytes at 0x08-0x3F. Reading it returns
 *          whatever the owner stored, so a byte with bit 7 set would be
 *          reported as a stopped oscillator forever; and *clearing* OSF after
 *          a write would silently corrupt one of those bytes. That second one
 *          is data loss in someone else's data.
 *   0x11 : DS3231 temperature. NVRAM again on a DS1307, so the temperature
 *          this driver has always reported on such a board was two arbitrary
 *          bytes formatted as degrees. Pre-existing, and gated here too.
 *   0x00 bit 7 : DS1307 CH (clock halt). Unused and always 0 on a DS3231, so
 *          checking it is harmless on both and meaningful on one.
 *
 * Identified from invariants rather than from a part number, because neither
 * part has one to read. Two independent DS3231-only facts have to hold: the
 * status register's bits 6:4 are always zero, and the temperature fraction
 * byte's bits 5:0 are always zero. NVRAM satisfying both by chance is one
 * count in 2^12 per byte pair, and a DS1307 whose NVRAM happens to is left
 * doing exactly what it did before this check existed. */
static bool g_is_ds3231;

static void i2c_rtc_identify_part(void) {
    g_is_ds3231 = false;
    uint8_t st, temp[2];
    if (!rtc_rd(DS3231_REG_STATUS, &st, 1)) return;
    if (!rtc_rd(DS3231_REG_TEMP, temp, 2)) return;
    if (st & 0x70) return;          /* reserved in the DS3231's status */
    if (temp[1] & 0x3F) return;     /* only bits 7:6 of the fraction exist */
    g_is_ds3231 = true;
}

void i2c_rtc_init(void) {
    /* The bus, not this driver's: i2c_bus_init() is idempotent, and asking
     * for it here rather than owning it is the whole of category E. Whichever
     * device is probed first brings the controller up; the rest find it
     * already running. */
    i2c_bus_init();
    i2c_rtc_identify_part();

    rtc_time_t tm;
    g_rtc_detected = false;

    /* A chip that answers but has lost its oscillator is a *detected* chip
     * with an unusable time -- a distinction the old code could not draw,
     * because it only ever asked for the time and a stopped DS3231 hands back
     * a plausible one. Detecting it here is what lets the clock be written
     * (which clears OSF) instead of the board deciding there is no RTC. */
    if (i2c_rtc_lost_power()) {
        /* The chip answered, so it is detected -- but its time is unverified
         * and does not become this system's clock. A DS3231 that lost its
         * oscillator reads 2000-01-01 00:00:00, which passes every range
         * check, so seeding from it would replace a known-unset clock with a
         * confidently wrong one. Nothing is worse to boot with.
         *
         * Not a diagnosis of the backup cell. OSF is sticky and this tree has
         * never cleared it, so on any board that has run before today it may
         * be reporting something old. Setting the clock -- by hand, from NTP,
         * or from the radio -- clears it, and from then on it means what it
         * says. */
        g_rtc_detected = true;
        printk("[I2C RTC] DS3231 at 0x68 answered, but OSF is set: its time is "
               "unverified and was not used. Set the clock to clear it "
               "(the flag is sticky and may be reporting an old event).\n");
        return;
    } else if (i2c_rtc_hw_read_time(&tm)) {
        if (tm.month >= 1 && tm.month <= 12 && tm.day >= 1 && tm.day <= 31 && tm.hour <= 23 && tm.min <= 59 && tm.sec <= 59) {
            g_rtc_detected = true;
            /* The DS3231 holds UTC, not local time (user, 2026-08-23). It is
             * storage for a clock that runs on UTC, and storing local time
             * there would make the hour that repeats every October
             * unrecoverable after a reset. A chip written by an older build
             * therefore reads an hour or two out until the next `date` or
             * (dcf-sync ... 1) rewrites it. */
            time_set_utc(&tm);
            char isostr[32];
            time_format_iso(&tm, isostr, sizeof(isostr));
#if defined(CONFIG_BOARD_RP2350)
            printk("[I2C RTC] %s detected at 0x68 (GP%d/GP%d)! Synced UTC: %s\n",
                   g_is_ds3231 ? "DS3231" : "DS1307-compatible",
                   CONFIG_I2C_RTC_SDA_GPIO, CONFIG_I2C_RTC_SCL_GPIO, isostr);
#else
            printk("[I2C RTC] %s detected at 0x68! Synced UTC: %s\n",
                   g_is_ds3231 ? "DS3231" : "DS1307-compatible", isostr);
#endif
            return;
        }
    }

    /* Two different absences, and saying which is the whole point.
     *
     * "Not found at 0x68" claims a probe. Where there is no controller
     * compiled in, i2c_probe_addr() and i2c_rtc_hw_read_time() are the stubs a
     * few hundred lines up, which return false without touching a wire, and
     * the sentence describes something that did not happen -- the same class
     * of line as the two the ESP32-P4's first boot caught in E2
     * (plan/phase27_esp32p4_bringup.md): drivers/at24c32.c announcing a "4KB
     * I2C EEPROM detected at 0x57!" from a build with no bus, and
     * drivers/usb_cdc.c naming two host device nodes from a stub.
     *
     * The gate was `#if defined(CONFIG_BOARD_RP2350)`, which was the same
     * thing as "has a controller" right up until E7 gave the P4 one -- after
     * which that board answered a full bus scan while printing "No I2C
     * controller on this target". Both sentences were true when written and
     * one of them stopped being true without changing. I2C_HAVE_CONTROLLER
     * (drivers/i2c_rtc.h) is the fact each site actually wanted.
     *
     * On the P4 the honest line still is "nothing at 0x68": that board has no
     * RTC chip at all. What it has is the LP domain's own counter and a
     * 32.768 kHz crystal, which is a real clock and nothing this driver has
     * ever heard of -- see E7 in the phase plan for why that is a different
     * device class rather than a second implementation of this one. */
    if (I2C_HAVE_CONTROLLER) {
#if defined(CONFIG_BOARD_RP2350)
        printk("[I2C RTC] No DS1307/DS3231 RTC module found at 0x68 (GP%d/GP%d); "
               "using the system software clock.\n",
               CONFIG_I2C_RTC_SDA_GPIO, CONFIG_I2C_RTC_SCL_GPIO);
#else
        printk("[I2C RTC] No DS1307/DS3231 RTC module answered at 0x68; "
               "using the system software clock.\n");
#endif
    } else {
        printk("[I2C RTC] No controller; kernel clock is software-only.\n");
    }
}

bool i2c_rtc_is_detected(void) {
    return g_rtc_detected;
}



/* --- The DS3231/DS1307 as an ordinary bus client (phase 30 category E) ---
 *
 * Every register access below goes through i2c_xfer(), exactly as
 * drivers/bme280.c's does. That is the whole of what "the RTC is a device on
 * the bus" means, and it is what lets the bus's own dispatcher stop knowing
 * what a DS3231 is.
 *
 * These used to call i2c_read_bytes()/i2c_write_bytes() -- the bus's *direct
 * hardware* helpers -- which meant the RTC reached the controller behind the
 * i2c task's back whenever it was not itself running inside that task, and
 * needed a second, U-mode implementation of its whole register map to work
 * when it was. i2c_xfer() routes to the task when one is alive and goes
 * direct before it exists, so one implementation covers both. */
static bool rtc_rd(uint8_t reg, uint8_t *dst, uint32_t len) {
    return i2c_xfer(DS1307_DS3231_I2C_ADDR, &reg, 1u, dst, len);
}

static bool rtc_wr(const uint8_t *buf, uint32_t len) {
    return i2c_xfer(DS1307_DS3231_I2C_ADDR, buf, len, NULL, 0u);
}

static bool i2c_rtc_hw_read_time(rtc_time_t *tm) {
    if (!tm) return false;
    uint8_t buf[7];
    if (!rtc_rd(0x00, buf, 7)) {
        return false;
    }

    /* CH is current state -- the DS1307's oscillator is halted *now* -- so a
     * read is genuinely meaningless and fails.
     *
     * OSF deliberately does NOT fail the read, and the distinction cost a
     * wrong conclusion before it was understood: **OSF is sticky**. The
     * DS3231 sets it when the oscillator stops and leaves it set until
     * software clears it, which nothing in this tree ever did -- so every
     * board here has it set, reporting an event that may be months old on a
     * chip that has kept perfect time ever since. Failing reads on it would
     * declare every working RTC in the fleet unusable.
     *
     * What it does mean is "this time has not been verified since the last
     * time the oscillator stopped", which is a caller's judgement to make:
     * i2c_rtc_init() declines to seed the *kernel* clock from it, and the
     * clock face lights a lamp. Writing the time clears the flag, so from
     * then on it means what it says. */
    if (buf[0] & 0x80) return false;              /* DS1307 CH: halted right now */

    tm->sec  = bcd2dec(buf[0] & 0x7F);
    tm->min  = bcd2dec(buf[1] & 0x7F);
    tm->hour = bcd2dec(buf[2] & 0x3F); // 24-hr mode
    // buf[3] is day of week (1-7)
    tm->day   = bcd2dec(buf[4] & 0x3F);
    tm->month = bcd2dec(buf[5] & 0x1F);
    tm->year  = 2000 + bcd2dec(buf[6]);
    tm->ms = 0;
    return true;
}

static bool i2c_rtc_hw_write_time(const rtc_time_t *tm) {
    if (!tm) return false;
    uint8_t reg_buf[8];
    reg_buf[0] = 0x00; // Register index
    reg_buf[1] = dec2bcd(tm->sec);
    reg_buf[2] = dec2bcd(tm->min);
    reg_buf[3] = dec2bcd(tm->hour);
    reg_buf[4] = 1; // Day of week default
    reg_buf[5] = dec2bcd(tm->day);
    reg_buf[6] = dec2bcd(tm->month);
    reg_buf[7] = dec2bcd((uint8_t)(tm->year >= 2000 ? (tm->year - 2000) : tm->year));

    if (!g_rtc_detected) return false;
    if (!rtc_wr(reg_buf, 8)) return false;

    /* Clear OSF now that the registers hold a real time again.
     *
     * The chip sets it and never clears it itself, so leaving it set would
     * make every subsequent read fail on a clock that is now correct -- the
     * flag would outlive the condition it reports. The datasheet prescribes
     * exactly this: write the time, then clear the flag.
     *
     * Best-effort: a part with no status register (a DS1307) NAKs the write
     * and the time is still set, which is the outcome that matters. */
    uint8_t st;
    if (g_is_ds3231 &&
        rtc_rd(DS3231_REG_STATUS, &st, 1) &&
        (st & DS3231_STATUS_OSF)) {
        uint8_t clear_buf[2] = { DS3231_REG_STATUS,
                                 (uint8_t)(st & (uint8_t)~DS3231_STATUS_OSF) };
        rtc_wr(clear_buf, 2);
    }
    return true;
}

static bool i2c_rtc_hw_read_temperature_c(int *temp_c) {
    if (!temp_c || !g_rtc_detected) return false;
    /* A DS1307 has no thermometer; 0x11 is two bytes of its NVRAM. This used
     * to format them as degrees. */
    if (!g_is_ds3231) return false;

    uint8_t buf[2];
    if (!rtc_rd(0x11, buf, 2)) {
        return false;
    }

    /* buf[0] = signed integer part; buf[1] bits 7:6 = quarter-degree
     * fraction, always a non-negative offset from buf[0] (the DS3231's own
     * representation -- e.g. -0.25C is integer=-1, fraction=0.75, not a
     * separately-signed fraction), so no extra sign handling is needed
     * here. */
    int temp_x4 = (int)(int8_t)buf[0] * 4 + (buf[1] >> 6);
    /* Round to nearest whole degree, half-away-from-zero. */
    *temp_c = (temp_x4 >= 0) ? (temp_x4 + 2) / 4 : (temp_x4 - 2) / 4;
    return true;
}

/* --- The public accessors ---------------------------------------------
 *
 * Each is now the device helper and nothing else. They used to open with an
 * `if (i2c_task_alive())` block marshalling a dedicated opcode -- one per
 * operation, each with a wire format, each implemented twice in the two
 * dispatchers -- and fall back to the direct helper when the task was not
 * running. All of that was the *bus's* job, and i2c_xfer() already does it:
 * routed through the task when one is alive, direct before it exists
 * (phase 30 category E).
 *
 * What that deletes is the reason this file had two copies of the DS3231's
 * register map, one of them in .utext for U-mode. The rule those comments
 * kept restating -- only i2c_rtc_init() and the task itself may touch the
 * bus -- is now structural: nothing here can touch it except through
 * i2c_xfer(). */
bool i2c_rtc_read_time(rtc_time_t *tm) {
    return i2c_rtc_hw_read_time(tm);
}

bool i2c_rtc_lost_power(void) {
    /* DS3231 only. On a DS1307 the address is user NVRAM and the answer would
     * be whatever the owner put there. */
    if (!g_is_ds3231) return false;

    /* Through the driver task once it exists, like every other public
     * accessor in this file.
     *
     * This used to touch the bus directly, breaking the rule stated above
     * i2c_rtc_hw_read_time(): only i2c_rtc_init() (before the task exists)
     * and the task itself may do that. Called from the clock application's
     * own task it raced the driver for the bus. A rule only some callers
     * follow is not a rule. */
    uint8_t st;
    if (!rtc_rd(DS3231_REG_STATUS, &st, 1)) return false;
    return (st & DS3231_STATUS_OSF) != 0;
}

bool i2c_rtc_write_time(const rtc_time_t *tm) {
    return i2c_rtc_hw_write_time(tm);
}

/* The last successful temperature reading, and when it was taken.
 *
 * A cache rather than a second reader, because the interesting consumer is
 * /proc/clock and that is served by the 9P task -- which has no business
 * touching the I2C bus. Whoever already owns the bus refreshes this; readers
 * get a value and its age and can judge for themselves whether it is stale.
 *
 * The age matters as much as the value: a temperature from an hour ago is
 * evidence about an hour ago, and correlating a crystal's rate against a stale
 * reading is how a spurious tempco gets published. */
static int      g_temp_cached_c;
static bool     g_temp_cached_ok;
static uint64_t g_temp_cached_ms;

bool i2c_rtc_cached_temperature_c(int *temp_c, uint32_t *age_s) {
    if (!g_temp_cached_ok) return false;
    if (temp_c) *temp_c = g_temp_cached_c;
    if (age_s) {
        uint64_t now = time_get_ms();
        *age_s = (uint32_t)((now > g_temp_cached_ms ? now - g_temp_cached_ms : 0) / 1000u);
    }
    return true;
}

static bool temp_cache(bool ok, int v) {
    if (ok) { g_temp_cached_c = v; g_temp_cached_ok = true; g_temp_cached_ms = time_get_ms(); }
    return ok;
}

bool i2c_rtc_read_temperature_c(int *temp_c) {
    if (!temp_c) return false;
    return temp_cache(i2c_rtc_hw_read_temperature_c(temp_c), *temp_c);
}