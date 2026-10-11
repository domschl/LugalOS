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
static rtc_chip_type_t g_rtc_type = RTC_TYPE_NONE;

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
 * They share an address (0x68) and a time-register layout (0x00-0x06), which
 * is why one driver has always served both -- but the registers beyond the
 * clock differ fundamentally:
 *
 *   0x00 bit 7: On DS1307, this is CH (Clock Halt). 1 = oscillator stopped.
 *               On DS3231, bit 7 is always 0.
 *   0x0F      : DS3231 status (OSF, bits 6:4 reserved as 0).
 *               On DS1307, this is battery-backed user NVRAM (56 bytes at 0x08-0x3F).
 *   0x11/0x12 : DS3231 temperature sensor (MSB integer deg C, LSB fraction).
 *               Crucially, on DS3231 registers 0x11/0x12 are hardware READ-ONLY,
 *               and fraction bits 5:0 are always 0.
 *               On DS1307, 0x11/0x12 are user NVRAM (read/write SRAM).
 *
 * Differentiation strategy:
 * 1. Read register 0x00. If 0x68 does not ACK, neither chip is present.
 * 2. If reg 0x00 bit 7 (CH) is 1, it is definitively a DS1307 (DS3231 bit 7 is always 0).
 * 3. Otherwise (when clock is running), test whether register 0x11 is writable SRAM
 *    or read-only temperature:
 *    Read original byte, write (orig ^ 0x5A), read back, and immediately restore orig.
 *    If readback matches (orig ^ 0x5A), register 0x11 is RAM -> DS1307.
 *    If readback does not match, register 0x11 is read-only -> DS3231.
 * 4. Verify DS3231 invariants: status 0x0F bits 6:4 == 0, temp fraction 0x12 bits 5:0 == 0.
 */
static void i2c_rtc_identify_part(void) {
    g_rtc_type = RTC_TYPE_NONE;

    uint8_t reg0 = 0;
    if (!rtc_rd(0x00, &reg0, 1)) {
        return; /* No device answered at 0x68 */
    }

    /* If bit 7 of reg 0 is set, CH is active: this is a halted DS1307. */
    if (reg0 & 0x80) {
        g_rtc_type = RTC_TYPE_DS1307;
        return;
    }

    /* Probe register 0x11: on DS1307 it is writable battery-backed SRAM.
     * On DS3231 it is the read-only temperature MSB register. */
    uint8_t orig11 = 0;
    if (rtc_rd(0x11, &orig11, 1)) {
        uint8_t test_val = (uint8_t)(orig11 ^ 0x5A);
        uint8_t wr_buf[2] = { 0x11, test_val };
        rtc_wr(wr_buf, 2);

        uint8_t rb = 0;
        rtc_rd(0x11, &rb, 1);

        /* Immediately restore original NVRAM content */
        uint8_t restore_buf[2] = { 0x11, orig11 };
        rtc_wr(restore_buf, 2);

        if (rb == test_val) {
            /* 0x11 stored and returned the written byte: NVRAM -> DS1307 */
            g_rtc_type = RTC_TYPE_DS1307;
            return;
        }
    }

    /* 0x11 ignored the write. Verify DS3231 register invariants:
     * - Status 0x0F bits 6:4 are 0
     * - Temp fraction 0x12 bits 5:0 are 0 */
    uint8_t st = 0, temp[2] = { 0 };
    if (rtc_rd(DS3231_REG_STATUS, &st, 1) && rtc_rd(DS3231_REG_TEMP, temp, 2)) {
        if ((st & 0x70) == 0 && (temp[1] & 0x3F) == 0) {
            g_rtc_type = RTC_TYPE_DS3231;
            return;
        }
    }

    /* Fallback if 0x68 answered but DS3231 invariants were not met */
    g_rtc_type = RTC_TYPE_DS1307;
}

void i2c_rtc_init(void) {
    /* The bus, not this driver's: i2c_bus_init() is idempotent, and asking
     * for it here rather than owning it is the whole of category E. Whichever
     * device is probed first brings the controller up; the rest find it
     * already running. */
    i2c_bus_init();
    i2c_rtc_identify_part();

    rtc_time_t tm;
    g_rtc_detected = (g_rtc_type != RTC_TYPE_NONE);

    if (!g_rtc_detected) {
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
        return;
    }

    const char *model = (g_rtc_type == RTC_TYPE_DS3231) ? "DS3231" : "DS1307";

    /* A chip that answers but has lost its oscillator is a *detected* chip
     * with an unusable time. Detecting it here is what lets the clock be written
     * (which clears OSF on DS3231, or clears CH and starts the oscillator on DS1307)
     * instead of the board deciding there is no RTC. */
    if (i2c_rtc_lost_power()) {
        if (g_rtc_type == RTC_TYPE_DS3231) {
            printk("[I2C RTC] DS3231 at 0x68 answered, but OSF is set: its time is "
                   "unverified and was not used. Set the clock to clear it "
                   "(the flag is sticky and may be reporting an old event).\n");
        } else {
            printk("[I2C RTC] DS1307 at 0x68 answered, but CH (clock halt) is set: its "
                   "oscillator is stopped. Set the clock with 'date' to start it.\n");
        }
        return;
    } else if (i2c_rtc_hw_read_time(&tm)) {
        if (tm.month >= 1 && tm.month <= 12 && tm.day >= 1 && tm.day <= 31 &&
            tm.hour <= 23 && tm.min <= 59 && tm.sec <= 59) {
            time_set_utc(&tm);
            char isostr[32];
            time_format_iso(&tm, isostr, sizeof(isostr));
#if defined(CONFIG_BOARD_RP2350)
            printk("[I2C RTC] %s detected at 0x68 (GP%d/GP%d)! Synced UTC: %s\n",
                   model, CONFIG_I2C_RTC_SDA_GPIO, CONFIG_I2C_RTC_SCL_GPIO, isostr);
#else
            printk("[I2C RTC] %s detected at 0x68! Synced UTC: %s\n",
                   model, isostr);
#endif
            return;
        } else {
            printk("[I2C RTC] %s at 0x68 answered, but stored time is out of range. "
                   "Set the clock to update it.\n", model);
            return;
        }
    }
}

bool i2c_rtc_is_detected(void) {
    return g_rtc_detected;
}

rtc_chip_type_t i2c_rtc_get_type(void) {
    return g_rtc_type;
}

const char *i2c_rtc_get_model_name(void) {
    switch (g_rtc_type) {
    case RTC_TYPE_DS3231: return "DS3231";
    case RTC_TYPE_DS1307: return "DS1307";
    default:              return "none";
    }
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
    if (!tm || !g_rtc_detected) return false;
    uint8_t buf[7];
    if (!rtc_rd(0x00, buf, 7)) {
        return false;
    }

    /* On DS1307, CH (bit 7 of register 0) is current state: the oscillator
     * is halted *now*, so the time registers are not running and the read fails.
     *
     * On DS3231, OSF in status register 0x0F is sticky (see i2c_rtc_init comment);
     * it does not fail this read. */
    if (g_rtc_type == RTC_TYPE_DS1307 && (buf[0] & 0x80)) {
        return false;
    }

    tm->sec  = bcd2dec(buf[0] & 0x7F);
    tm->min  = bcd2dec(buf[1] & 0x7F);

    /* 12-hour vs 24-hour mode (bit 6 of reg 0x02) */
    if (buf[2] & 0x40) {
        uint8_t h = bcd2dec(buf[2] & 0x1F);
        bool pm = (buf[2] & 0x20) != 0;
        if (pm && h < 12) h += 12;
        else if (!pm && h == 12) h = 0;
        tm->hour = h;
    } else {
        tm->hour = bcd2dec(buf[2] & 0x3F);
    }

    // buf[3] is day of week (1-7)
    tm->day   = bcd2dec(buf[4] & 0x3F);
    tm->month = bcd2dec(buf[5] & 0x1F);
    tm->year  = 2000 + bcd2dec(buf[6]);
    tm->ms = 0;
    return true;
}

static bool i2c_rtc_hw_write_time(const rtc_time_t *tm) {
    if (!tm || !g_rtc_detected) return false;
    uint8_t reg_buf[8];
    reg_buf[0] = 0x00; // Register index
    /* Bit 7 = 0: on DS1307, writing bit 7 = 0 clears CH and starts the oscillator.
     * On DS3231, bit 7 is always 0. */
    reg_buf[1] = dec2bcd(tm->sec) & 0x7F;
    reg_buf[2] = dec2bcd(tm->min) & 0x7F;
    reg_buf[3] = dec2bcd(tm->hour) & 0x3F; // 24-hr mode
    reg_buf[4] = 1; // Day of week default
    reg_buf[5] = dec2bcd(tm->day) & 0x3F;
    reg_buf[6] = dec2bcd(tm->month) & 0x1F;
    reg_buf[7] = dec2bcd((uint8_t)(tm->year >= 2000 ? (tm->year - 2000) : tm->year));

    if (!rtc_wr(reg_buf, 8)) return false;

    /* For DS3231: clear OSF in status register now that real time is loaded.
     * For DS1307: writing bit 7 = 0 to register 0x00 above already cleared CH. */
    if (g_rtc_type == RTC_TYPE_DS3231) {
        uint8_t st;
        if (rtc_rd(DS3231_REG_STATUS, &st, 1) && (st & DS3231_STATUS_OSF)) {
            uint8_t clear_buf[2] = { DS3231_REG_STATUS,
                                     (uint8_t)(st & (uint8_t)~DS3231_STATUS_OSF) };
            rtc_wr(clear_buf, 2);
        }
    }
    return true;
}

static bool i2c_rtc_hw_read_temperature_c(int *temp_c) {
    if (!temp_c || !g_rtc_detected) return false;
    /* A DS1307 has no thermometer; 0x11 is user NVRAM. */
    if (g_rtc_type != RTC_TYPE_DS3231) return false;

    uint8_t buf[2];
    if (!rtc_rd(DS3231_REG_TEMP, buf, 2)) {
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
    if (!g_rtc_detected) return false;

    if (g_rtc_type == RTC_TYPE_DS3231) {
        uint8_t st;
        if (!rtc_rd(DS3231_REG_STATUS, &st, 1)) return false;
        return (st & DS3231_STATUS_OSF) != 0;
    } else if (g_rtc_type == RTC_TYPE_DS1307) {
        /* DS1307: bit 7 of register 0x00 is CH (Clock Halt).
         * 1 = oscillator stopped / lost power. */
        uint8_t sec;
        if (!rtc_rd(0x00, &sec, 1)) return false;
        return (sec & 0x80) != 0;
    }
    return false;
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