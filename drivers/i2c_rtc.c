#include "drivers/i2c_rtc.h"
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

#if defined(CONFIG_BOARD_RP2350)
/* Board-fact-driven (L3, plan/phase11_pico_clock_green.md) rather than the
 * GP4/GP5/I2C0 literals this file hardcoded before: the Pico-Clock-Green
 * baseboard's DS3231 is wired to GP6/GP7, which RP2350's GPIO-to-
 * controller mapping (alternates every 4 pins) puts on the I2C1
 * peripheral instance, not I2C0 -- a different base address, not just
 * different pins. cmake/board-rp2350.cmake keeps the original GP4/GP5/
 * I2C0 values (CONFIG_I2C_RTC_BASE == I2C0's 0x40090000); cmake/board-
 * rp2350-clock.cmake sets GP6/GP7/I2C1 (0x40098000) instead. */
#define I2C_SDA_PIN CONFIG_I2C_RTC_SDA_GPIO
#define I2C_SCL_PIN CONFIG_I2C_RTC_SCL_GPIO

#define RESETS_BASE            0x40020000UL
#define RESETS_RESET           (RESETS_BASE + 0x0000)
#define RESETS_RESET_SET       (RESETS_BASE + 0x2000) // Atomic Bit SET Alias
#define RESETS_RESET_CLR       (RESETS_BASE + 0x3000) // Atomic Bit CLR Alias
#define RESETS_RESET_DONE      (RESETS_BASE + 0x0008) // Reset Done Register --
    // was 0x000C (off by one register); verified against
    // ~/gith/pico/pico-sdk/src/rp2350/hardware_regs/include/hardware/regs/
    // resets.h's RESETS_RESET_DONE_OFFSET while researching L2's own ADC
    // reset sequence. Likely dormant rather than actually broken: the
    // 10000-iteration poll below still burns enough real time for the
    // peripheral's (near-instant) unreset to finish underneath it
    // regardless of which address it polled, so this was never observed
    // to misbehave -- but it's the wrong register, so fixed outright now
    // that it's been found, not left in place because it happened to work.

/* RESETS_RESET_I2C0 is bit 4, RESETS_RESET_I2C1 is bit 5 (resets.h) --
 * derived from CONFIG_I2C_RTC_BASE rather than added as its own board
 * fact, so a board file can't get this one wrong independently of the
 * base address it already has to get right. */
#if CONFIG_I2C_RTC_BASE == 0x40098000UL
#define I2C_RTC_RESET_BIT (1u << 5) // RESETS_RESET_I2C1
#else
#define I2C_RTC_RESET_BIT (1u << 4) // RESETS_RESET_I2C0
#endif

/* M5 Phase 3, plan/phase12_microkernel_migration.md: RP2350's Secure/
 * Non-secure split -- the same mechanism found for GPIO in M5 Phase 1
 * (drivers/uart_rp2350.c's ACCESSCTRL_GPIO_NSMASK0 comment has the full
 * datasheet citation, not repeated here) -- also gates I2C0/I2C1, but
 * through a differently-shaped register: one register per peripheral
 * (not one bit per GPIO), with SP/SU/NSP/NSU bits (Secure/Non-secure x
 * Privileged/Unprivileged). Checked directly against
 * ~/gith/pico/pico-sdk's accessctrl.h: reset value 0xfc, i.e. Secure
 * access enabled (SP=1) and Non-secure access disabled (NSP=NSU=0) by
 * default. U-mode is Non-secure+Unprivileged -- NSU -- and that header's
 * own comment notes NSU "is writable... if and only if NSP is set", so
 * both bits need setting together, from M-mode, before the task exists
 * (i2c_hw_init() below). Same conditional shape as I2C_RTC_RESET_BIT
 * above, for the same reason. */
#define ACCESSCTRL_BASE 0x40060000UL
#if CONFIG_I2C_RTC_BASE == 0x40098000UL
#define ACCESSCTRL_I2C_RTC (ACCESSCTRL_BASE + 0x88) // ACCESSCTRL_I2C1
#else
#define ACCESSCTRL_I2C_RTC (ACCESSCTRL_BASE + 0x84) // ACCESSCTRL_I2C0
#endif
#define ACCESSCTRL_I2C_NSP (1u << 1)
#define ACCESSCTRL_I2C_NSU (1u << 0)

/* Found the hard way, as a boot-time bus fault, immediately after adding
 * the write below without it: every ACCESSCTRL register *except*
 * GPIO_NSMASK0/1 (the two heartbeat's own fix, drivers/uart_rp2350.c,
 * happened to use) requires the 16-bit value 0xacce present in the
 * write's upper 16 bits, or the write both fails *and* raises a bus
 * fault rather than silently doing nothing -- straight from the
 * datasheet's own ACCESSCTRL overview section, not something either of
 * this tree's two prior ACCESSCTRL fixes (GPIO_NSMASK0, both exempt) had
 * ever needed to learn. */
#define ACCESSCTRL_WRITE_PASSWORD 0xacce0000UL

#define IO_BANK0_BASE          0x40028000UL
#define IO_BANK0_CTRL(n)       (IO_BANK0_BASE + 0x004 + (n) * 8)

#define PADS_BANK0_BASE        0x40038000UL
#define PADS_BANK0_PAD(n)      (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define I2C_RTC_BASE            ((uintptr_t)CONFIG_I2C_RTC_BASE)
#define IC_CON                 (I2C_RTC_BASE + 0x00)
#define IC_TAR                 (I2C_RTC_BASE + 0x04)
#define IC_DATA_CMD            (I2C_RTC_BASE + 0x10)
#define IC_SS_SCL_HCNT         (I2C_RTC_BASE + 0x14)
#define IC_SS_SCL_LCNT         (I2C_RTC_BASE + 0x18)
#define IC_FS_SCL_HCNT         (I2C_RTC_BASE + 0x1C)
#define IC_FS_SCL_LCNT         (I2C_RTC_BASE + 0x20)
#define IC_INTR_STAT           (I2C_RTC_BASE + 0x2C)
#define IC_RAW_INTR_STAT       (I2C_RTC_BASE + 0x34)
#define IC_CLR_TX_ABRT         (I2C_RTC_BASE + 0x54)
#define IC_ENABLE              (I2C_RTC_BASE + 0x6C)
#define IC_STATUS              (I2C_RTC_BASE + 0x70)
#define IC_TXFLR               (I2C_RTC_BASE + 0x74)
#define IC_RXFLR               (I2C_RTC_BASE + 0x78)
#define IC_SDA_HOLD            (I2C_RTC_BASE + 0x7C)
#define IC_TX_ABRT_SOURCE      (I2C_RTC_BASE + 0x80)
#define IC_FS_SPKLEN           (I2C_RTC_BASE + 0xA0)

#define REG(addr) (*(volatile uint32_t *)(addr))

static void i2c_hw_init(void) {
    /* 1. Assert and then clear the peripheral's reset using RP2350 Atomic
     * Alias Registers (I2C0 or I2C1, whichever CONFIG_I2C_RTC_BASE says) */
    REG(RESETS_RESET_SET) = I2C_RTC_RESET_BIT;
    for (volatile int i = 0; i < 1000; i++);
    REG(RESETS_RESET_CLR) = I2C_RTC_RESET_BIT;
    int timeout = 10000;
    while (!(REG(RESETS_RESET_DONE) & I2C_RTC_RESET_BIT) && --timeout > 0);

    /* M5 Phase 3: the I2C controller needs to be Non-secure-accessible for
     * the U-mode task's own serve loop below to actually touch it -- must
     * happen here, from M-mode, before the task exists. See
     * ACCESSCTRL_I2C_RTC's own comment above -- including the write
     * password prefix that comment explains. */
    REG(ACCESSCTRL_I2C_RTC) = ACCESSCTRL_WRITE_PASSWORD | REG(ACCESSCTRL_I2C_RTC)
                              | ACCESSCTRL_I2C_NSP | ACCESSCTRL_I2C_NSU;

    /* 2. Configure GP4 (SDA) & GP5 (SCL) strictly as Function 3 (I2C) */
    REG(IO_BANK0_CTRL(I2C_SDA_PIN)) = 3;
    REG(IO_BANK0_CTRL(I2C_SCL_PIN)) = 3;

    /* 3. Enable Pull-ups, Input Enable, Schmitt Trigger on GP4 & GP5 pads (0x5A) */
    REG(PADS_BANK0_PAD(I2C_SDA_PIN)) = 0x5A;
    REG(PADS_BANK0_PAD(I2C_SCL_PIN)) = 0x5A;

    /* 4. Disable I2C0 before configuring */
    REG(IC_ENABLE) = 0;

    /* Master mode (bit 0), 7-bit addressing, Fast mode (2u << 1), Restart enable (bit 6), Slave disable (bit 5), TX_EMPTY_CTRL (bit 8) */
    REG(IC_CON) = (1u << 0) | (1u << 5) | (2u << 1) | (1u << 6) | (1u << 8);
    REG(IC_TAR) = DS1307_DS3231_I2C_ADDR;

    /* 100kHz Standard Mode Clock Dividers for 150MHz system clock */
    uint32_t freq_in = 150000000;
    uint32_t baudrate = 100000; // 100 kHz
    uint32_t period = (freq_in + baudrate / 2) / baudrate; // 1500 cycles
    uint32_t lcnt = period * 3 / 5; // 900
    uint32_t hcnt = period - lcnt;  // 600

    REG(IC_FS_SCL_HCNT) = hcnt;
    REG(IC_FS_SCL_LCNT) = lcnt;
    REG(IC_SS_SCL_HCNT) = hcnt;
    REG(IC_SS_SCL_LCNT) = lcnt;

    REG(IC_FS_SPKLEN) = lcnt < 16 ? 1 : lcnt / 16;

    /* Critical 300ns SDA Hold Time for 150MHz system clock (matching Pico SDK) */
    uint32_t sda_tx_hold_count = ((freq_in * 3) / 10000000) + 1; // 46 cycles = 307ns
    REG(IC_SDA_HOLD) = sda_tx_hold_count;

    /* Enable I2C0 */
    REG(IC_ENABLE) = 1;
}

static bool i2c_write_bytes(uint8_t addr, const uint8_t *src, int len) {
    REG(IC_ENABLE) = 0;
    REG(IC_TAR) = addr;
    REG(IC_ENABLE) = 1;
    (void)REG(IC_CLR_TX_ABRT);

    bool abort = false;
    for (int i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = src[i];
        if (last) cmd |= (1u << 9); // STOP bit

        int timeout = 10000;
        while (!(REG(IC_STATUS) & (1u << 1)) && --timeout > 0); // Wait TX Not Full
        if (timeout == 0) return false;

        REG(IC_DATA_CMD) = cmd;

        timeout = 10000;
        do {
            if (REG(IC_RAW_INTR_STAT) & (1u << 6)) { // TX_ABRT (bit 6)
                abort = true;
                (void)REG(IC_CLR_TX_ABRT);
                break;
            }
        } while (--timeout > 0 && !(REG(IC_RAW_INTR_STAT) & (1u << 4))); // TX_EMPTY (bit 4)

        if (abort || timeout == 0) return false;
    }
    return !abort;
}

static bool i2c_read_bytes(uint8_t addr, uint8_t reg, uint8_t *dst, int len) {
    // 1. Write register index first
    if (!i2c_write_bytes(addr, &reg, 1)) return false;

    // 2. Read bytes
    REG(IC_ENABLE) = 0;
    REG(IC_TAR) = addr;
    REG(IC_ENABLE) = 1;
    (void)REG(IC_CLR_TX_ABRT);

    bool abort = false;
    for (int i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = (1u << 8); // READ bit (bit 8)
        if (last) cmd |= (1u << 9); // STOP bit (bit 9)

        int timeout = 10000;
        while (!(REG(IC_STATUS) & (1u << 1)) && --timeout > 0); // Wait TX Not Full
        if (timeout == 0) return false;

        REG(IC_DATA_CMD) = cmd;

        timeout = 10000;
        do {
            if (REG(IC_RAW_INTR_STAT) & (1u << 6)) { // TX_ABRT (bit 6)
                abort = true;
                (void)REG(IC_CLR_TX_ABRT);
                break;
            }
        } while (--timeout > 0 && (REG(IC_STATUS) & (1u << 3)) == 0); // Wait RX Not Empty (bit 3)

        if (abort || timeout == 0) return false;

        dst[i] = (uint8_t)REG(IC_DATA_CMD);
    }
    return !abort;
}

static bool i2c_probe_addr(uint8_t addr) {
    REG(IC_ENABLE) = 0;
    REG(IC_TAR) = addr;
    REG(IC_ENABLE) = 1;
    (void)REG(IC_CLR_TX_ABRT);

    // Send READ command + STOP bit (matching Pico SDK probe)
    uint32_t cmd = (1u << 8) | (1u << 9);
    REG(IC_DATA_CMD) = cmd;

    int timeout = 10000;
    bool abort = false;
    do {
        if (REG(IC_RAW_INTR_STAT) & (1u << 6)) { // TX_ABRT
            abort = true;
            (void)REG(IC_CLR_TX_ABRT);
            break;
        }
    } while (--timeout > 0 && (REG(IC_STATUS) & (1u << 3)) == 0); // RXFLR / RFNE

    if (!abort && timeout > 0) {
        (void)REG(IC_DATA_CMD); // Drain byte from RX FIFO
        return true; // ACK!
    }
    return false;
}
#else
static void i2c_hw_init(void) {}
static bool i2c_probe_addr(uint8_t addr) { (void)addr; return false; }
static bool i2c_write_bytes(uint8_t addr, const uint8_t *src, int len) {
    (void)addr; (void)src; (void)len; return false;
}
static bool i2c_read_bytes(uint8_t addr, uint8_t reg, uint8_t *dst, int len) {
    (void)addr; (void)reg; (void)dst; (void)len; return false;
}
#endif

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
static bool i2c_rtc_hw_read_time(rtc_time_t *tm);
static bool i2c_rtc_hw_write_time(const rtc_time_t *tm);
static bool i2c_rtc_hw_read_temperature_c(int *temp_c);

void i2c_rtc_init(void) {
    i2c_hw_init();

    rtc_time_t tm;
    g_rtc_detected = false;

    /* A chip that answers but has lost its oscillator is a *detected* chip
     * with an unusable time -- a distinction the old code could not draw,
     * because it only ever asked for the time and a stopped DS3231 hands back
     * a plausible one. Detecting it here is what lets the clock be written
     * (which clears OSF) instead of the board deciding there is no RTC. */
    if (i2c_rtc_lost_power()) {
        g_rtc_detected = true;
        printk("[I2C RTC] DS3231 present but its oscillator stopped (OSF set): "
               "the stored time is not usable. Backup cell? Set the clock to "
               "clear it.\n");
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
            printk("[I2C RTC] DS1307/DS3231 detected at 0x68 (GP%d/GP%d)! Synced UTC: %s\n",
                   CONFIG_I2C_RTC_SDA_GPIO, CONFIG_I2C_RTC_SCL_GPIO, isostr);
#else
            printk("[I2C RTC] DS1307/DS3231 detected at 0x68! Synced UTC: %s\n", isostr);
#endif
            return;
        }
    }

    printk("[I2C RTC] No DS1307/DS3231 RTC module found at 0x68 (Using system software clock).\n");
}

bool i2c_rtc_is_detected(void) {
    return g_rtc_detected;
}

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

bool i2c_rtc_lost_power(void) {
    uint8_t st;
    if (!i2c_read_bytes(DS1307_DS3231_I2C_ADDR, DS3231_REG_STATUS, &st, 1)) return false;
    return (st & DS3231_STATUS_OSF) != 0;
}

static bool i2c_rtc_hw_read_time(rtc_time_t *tm) {
    if (!tm) return false;
    uint8_t buf[7];
    if (!i2c_read_bytes(DS1307_DS3231_I2C_ADDR, 0x00, buf, 7)) {
        return false;
    }

    /* A stopped oscillator means the seven bytes above describe nothing. Fail
     * the read rather than hand back a plausible wrong time -- a caller that
     * gets `false` falls back to whatever else it knows, and a caller that
     * gets 2000-01-01 believes it. */
    if (buf[0] & 0x80) return false;              /* DS1307 CH: clock halted */
    if (i2c_rtc_lost_power()) return false;       /* DS3231 OSF */

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
    if (!i2c_write_bytes(DS1307_DS3231_I2C_ADDR, reg_buf, 8)) return false;

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
    if (i2c_read_bytes(DS1307_DS3231_I2C_ADDR, DS3231_REG_STATUS, &st, 1) &&
        (st & DS3231_STATUS_OSF)) {
        uint8_t clear_buf[2] = { DS3231_REG_STATUS,
                                 (uint8_t)(st & (uint8_t)~DS3231_STATUS_OSF) };
        i2c_write_bytes(DS1307_DS3231_I2C_ADDR, clear_buf, 2);
    }
    return true;
}

static bool i2c_rtc_hw_read_temperature_c(int *temp_c) {
    if (!temp_c || !g_rtc_detected) return false;

    uint8_t buf[2];
    if (!i2c_read_bytes(DS1307_DS3231_I2C_ADDR, 0x11, buf, 2)) {
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

void i2c_scan_bus(void) {
    /* An answer to a typed `i2c` command, so it goes to the console stream
     * (C0). Using printk() put the whole scan table into the kernel log
     * ring, where it turned up again in `cat /proc/kmsg`. */
#if defined(CONFIG_BOARD_RP2350)
    cprintf("\nI2C Bus Scan (GP%d SDA / GP%d SCL):\n", CONFIG_I2C_RTC_SDA_GPIO, CONFIG_I2C_RTC_SCL_GPIO);
#else
    cprintf("\nI2C Bus Scan:\n");
#endif
    cprintf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

    int found_count = 0;
    for (int row = 0; row < 128; row += 16) {
        cprintf("%02x: ", row);
        for (int col = 0; col < 16; col++) {
            uint8_t addr = row + col;
            if (addr < 0x08 || addr > 0x77) {
                cprintf("   ");
            } else {
                if (i2c_probe_addr(addr)) {
                    cprintf("%02x ", addr);
                    found_count++;
                } else {
                    cprintf("-- ");
                }
            }
        }
        cprintf("\n");
    }
    cprintf("Found %d I2C device(s).\n\n", found_count);
}

/* M4.5, plan/phase12_microkernel_migration.md, Part B: RTC and EEPROM share
 * one physical I2C bus, so this one "i2c" task serves both -- see i2c_rtc.h
 * for the fuller reasoning. Wire protocol, one opcode byte then a
 * fixed-shape payload per op; unlike drivers/spisd_rp2350.c's BLK_REQ_*
 * (a real wire onto an SD card), addr/len/temperature here are plain native
 * types, copied as-is between endpoint-owned buffers in the same address
 * space -- there is no external byte order to defend against.
 *
 *   'T' (read time)        req: [op]                    resp: [ok(1)] + rtc_time_t
 *   'S' (write time)       req: [op] + rtc_time_t        resp: [ok(1)]
 *   'C' (read temperature) req: [op]                     resp: [ok(1)] + int(4)
 *   'R' (EEPROM read)      req: [op] + addr(2) + len(2)  resp: [result:int32(4)] + data
 *   'X' (EEPROM write)     req: [op] + addr(2) + len(2) + data  resp: [result:int32(4)]
 *
 * The EEPROM ops' buffers were originally sized to the whole 4KB AT24C32
 * device; M5 Phase 3, plan/phase12_microkernel_migration.md, capped a
 * single EEPROM read/write at AT24C32_CHUNK_MAX (drivers/at24c32.h) bytes
 * instead -- far more than a syscall-sized U-mode buffer should carry, and
 * the reason i2c was deferred when tm1638 converted to U-mode in Phase 2.
 * drivers/at24c32.c's at24c32_read()/at24c32_write() loop internally in
 * chunks this size for anything larger, so this is invisible to every
 * existing caller. */
#define I2C_OP_RTC_READ_TIME  ((uint8_t)'T')
#define I2C_OP_RTC_WRITE_TIME ((uint8_t)'S')
#define I2C_OP_RTC_READ_TEMP  ((uint8_t)'C')
#define I2C_OP_EE_READ        ((uint8_t)'R')
#define I2C_OP_EE_WRITE       ((uint8_t)'X')

#define I2C_REQ_CAP  (5u + AT24C32_CHUNK_MAX)
#define I2C_RESP_CAP (4u + AT24C32_CHUNK_MAX)

static uint8_t         g_i2c_req[I2C_REQ_CAP];
static uint8_t         g_i2c_resp[I2C_RESP_CAP];
static chan_endpoint_t *g_i2c_ep;
static int              g_i2c_task_pid = -1;

/* M4.5 verify: counts chan_call()s actually served -- see
 * drivers/uart_16550.c's g_uart_write_calls comment for the reasoning. */
static uint32_t g_i2c_calls;

uint32_t i2c_task_call_count(void) { return g_i2c_calls; }

bool i2c_task_alive(void) {
    if (g_i2c_task_pid < 0) return false;
    int st = sched_task_state(g_i2c_task_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

/* --------------------------------------------------- the RTC wire ------ */

/*
 * Nine bytes, laid out explicitly:
 *
 *   [0..1] year, BIG-endian   [2] month  [3] day
 *   [4] hour   [5] min   [6] sec        [7..8] ms, big-endian
 *
 * Explicit because the U-mode server above cannot call anything outside
 * .utext and so decodes these bytes by hand -- which means the client has to
 * agree with a *byte layout*, not with a struct.
 *
 * It used to `memcpy()` an `rtc_time_t` instead, which agreed with neither
 * server. `rtc_time_t` is little-endian and carries a pad byte before `ms`,
 * so the U-mode server read the year byte-swapped: 2026 (0x07EA) arrived as
 * 0xEA07 = 59911, and `(uint8_t)(59911 - 2000)` is 55, written as BCD 0x55
 * and read back as the year 2055. Month and day, being single bytes, were
 * perfectly correct the whole time -- which is exactly what made it look like
 * a bad RTC chip rather than a bad wire format (found on hardware
 * 2026-08-23, on a clock the DCF-77 receiver had just set correctly).
 *
 * The read direction was broken differently and more quietly: the server
 * replies with 1 + 9 bytes and the client demanded 1 + sizeof(rtc_time_t) =
 * 1 + 10, so the check never passed and *every* read silently fell through to
 * direct hardware access from whatever task asked. It worked, which is why
 * nobody noticed, but it meant the display task was driving the I2C bus
 * itself and the i2c task was serving nothing.
 *
 * The lesson is the one i2c_usys_put_i32()'s comment already records from the
 * other direction: a wire format is a format, not a struct.
 */
#define RTC_WIRE_LEN 9u

static void rtc_to_wire(const rtc_time_t *tm, uint8_t *w) {
    w[0] = (uint8_t)(tm->year >> 8);
    w[1] = (uint8_t)tm->year;
    w[2] = tm->month;
    w[3] = tm->day;
    w[4] = tm->hour;
    w[5] = tm->min;
    w[6] = tm->sec;
    w[7] = (uint8_t)(tm->ms >> 8);
    w[8] = (uint8_t)tm->ms;
}

static void rtc_from_wire(const uint8_t *w, rtc_time_t *tm) {
    tm->year  = (uint16_t)(((uint16_t)w[0] << 8) | w[1]);
    tm->month = w[2];
    tm->day   = w[3];
    tm->hour  = w[4];
    tm->min   = w[5];
    tm->sec   = w[6];
    tm->ms    = (uint16_t)(((uint16_t)w[7] << 8) | w[8]);
}

/* Only RP2350 has real I2C hardware to isolate -- the #else branch below
 * (QEMU rv64/rv32) keeps the plain kernel-mode server every M4.5 driver
 * task had before M5, unchanged. Unlike drivers/uart_rp2350.c/

 * tm1638_rp2350.c (entirely separate, RP2350-only files), i2c_rtc.c is
 * shared across every target, so the split lives inside this one file. */
#if defined(CONFIG_BOARD_RP2350)

/* ---- U-mode implementation, M5 Phase 3, plan/phase12_microkernel_migration.md ----
 *
 * A second, independent implementation of the I2C register handshake
 * above (i2c_write_bytes/i2c_read_bytes) and drivers/at24c32.c's
 * (i2c_write_at24/i2c_read_at24, at24c32_hw_write()'s page-boundary
 * chunking), tagged I2C_UATTR and reachable only from the U-mode task's
 * own serve loop below -- not a refactor of the existing kernel-mode
 * ones into something shared. Those keep serving the direct-hardware
 * fallback path exactly as before, unreachable from U-mode. The two
 * copies never run concurrently -- the facade functions route to one or
 * the other depending on i2c_task_alive() -- so nothing needs to agree
 * between them beyond the wire protocol both sides already share.
 *
 * Unlike drivers/tm1638_rp2350.c's four separate bit-bang primitives,
 * RTC's and EEPROM's I2C transactions are the same DesignWare handshake
 * with different address-byte counts (RTC: 1-byte register pointer;
 * EEPROM: 2-byte memory address), so this is two generalized primitives
 * instead of four duplicated ones. */
#define I2C_UATTR __attribute__((section(".utext"))) __attribute__((no_sanitize("undefined")))

/* Hand-rolled per translation unit, not shared with drivers/uart_rp2350.c's
 * or drivers/tm1638_rp2350.c's own usys_*() stubs or user/progs/usys.h --
 * an I2C_UATTR function must not call anything the compiler might place
 * outside .utext, and a cross-file inline is not a guarantee. */
__attribute__((always_inline)) static inline void i2c_usys_delay_us(long us) {
    register long r_a0 __asm__("a0") = SYS_DELAY_US;
    register long r_a1 __asm__("a1") = us;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1) : "memory");
}
__attribute__((always_inline)) static inline long i2c_usys_chan_serve_wait(const char *name, uint8_t *buf, long buf_max) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_WAIT;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = buf_max;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}
__attribute__((always_inline)) static inline long i2c_usys_chan_serve_reply(const char *name, const uint8_t *buf, long len) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_REPLY;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = len;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}
/* Tiny one-liners, but still hand-rolled/always_inline rather than reused
 * from the kernel-mode bcd2dec()/dec2bcd() above (get_u16()/put_i32()
 * were only ever used by the kernel-mode i2c_task_body() this replaces,
 * and are gone with it) -- same reasoning as the syscall stubs, applied
 * to arithmetic helpers too:
 * "obviously inlined" is an optimizer heuristic, not a structural
 * guarantee (drivers/uart_rp2350.c's heartbeat_usleep_until() comment has
 * the fuller story of finding that out the hard way). */
__attribute__((always_inline)) static inline uint8_t i2c_usys_bcd2dec(uint8_t v) { return ((v >> 4) * 10) + (v & 0x0F); }
__attribute__((always_inline)) static inline uint8_t i2c_usys_dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
__attribute__((always_inline)) static inline uint16_t i2c_usys_get_u16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
__attribute__((always_inline)) static inline void i2c_usys_put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
/* Native (little-endian) byte order, NOT the big-endian i2c_usys_put_u16()
 * above uses -- matches the client side's get_i32() (drivers/at24c32.c),
 * which decodes via a raw memcpy() of the wire bytes into an int32_t and
 * so is native-endian by construction, same as the original kernel-mode
 * put_i32() this replaces (also a memcpy()). Found on real hardware, not
 * predicted: writing this field big-endian, matching put_u16() instead of
 * matching get_i32(), sent (eeprom-write)'s own 15-byte result back as
 * 251658240 -- the client reading 0x0F000000 where the wire held
 * 0x0000000F. */
__attribute__((always_inline)) static inline void i2c_usys_put_i32(uint8_t *p, int32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

I2C_UATTR static void i2c_usys_target(uint8_t addr) {
    REG(IC_ENABLE) = 0;
    REG(IC_TAR) = addr;
    REG(IC_ENABLE) = 1;
    (void)REG(IC_CLR_TX_ABRT);
}

/* Writes len bytes already TAR-targeted by the caller, STOP after the
 * last byte only if stop_at_end -- false is for a register/address
 * prefix that a read phase (i2c_usys_read_reg() below) continues past. */
I2C_UATTR static bool i2c_usys_write_raw(const uint8_t *data, int len, bool stop_at_end) {
    for (int i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = data[i];
        if (last && stop_at_end) cmd |= (1u << 9);

        int timeout = 10000;
        while (!(REG(IC_STATUS) & (1u << 1)) && --timeout > 0);
        if (timeout == 0) return false;

        REG(IC_DATA_CMD) = cmd;

        timeout = 10000;
        bool abort = false;
        do {
            if (REG(IC_RAW_INTR_STAT) & (1u << 6)) {
                abort = true;
                (void)REG(IC_CLR_TX_ABRT);
                break;
            }
        } while (--timeout > 0 && !(REG(IC_RAW_INTR_STAT) & (1u << 4)));

        if (abort || timeout == 0) return false;
    }
    return true;
}

/* Sends len bytes to addr, STOP after the last -- RTC's register-pointer
 * + payload writes and EEPROM's 2-byte-address + payload writes alike. */
I2C_UATTR static bool i2c_usys_write_bytes(uint8_t addr, const uint8_t *data, int len) {
    i2c_usys_target(addr);
    return i2c_usys_write_raw(data, len, true);
}

/* Writes reg_len address/register bytes (no STOP), then re-targets and
 * reads len bytes -- RTC's 1-byte register reads (time at 0x00,
 * temperature at 0x11) and EEPROM's 2-byte address reads alike. */
I2C_UATTR static bool i2c_usys_read_reg(uint8_t addr, const uint8_t *reg, int reg_len, uint8_t *dst, int len) {
    i2c_usys_target(addr);
    if (!i2c_usys_write_raw(reg, reg_len, false)) return false;

    i2c_usys_target(addr);
    for (int i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = (1u << 8);
        if (last) cmd |= (1u << 9);

        int timeout = 10000;
        while (!(REG(IC_STATUS) & (1u << 1)) && --timeout > 0);
        if (timeout == 0) return false;

        REG(IC_DATA_CMD) = cmd;

        timeout = 10000;
        bool abort = false;
        do {
            if (REG(IC_RAW_INTR_STAT) & (1u << 6)) {
                abort = true;
                (void)REG(IC_CLR_TX_ABRT);
                break;
            }
        } while (--timeout > 0 && (REG(IC_STATUS) & (1u << 3)) == 0);

        if (abort || timeout == 0) return false;

        dst[i] = (uint8_t)REG(IC_DATA_CMD);
    }
    return true;
}

/* EEPROM writes cross AT24C32_PAGE_SIZE (32-byte) boundaries in separate
 * transactions with a real ~10ms page-write cycle between them -- the
 * same chunking drivers/at24c32.c's at24c32_hw_write() already does, via
 * SYS_DELAY_US instead of time_delay_us() (which touches hardware no
 * U-mode domain has ever needed to be granted, and, on this build,
 * services usb_cdc_task() inline inside its own busy loop -- see
 * arch/riscv/common/trap.c's own SYS_DELAY_US comment). AT24C32_CHUNK_MAX
 * (drivers/at24c32.h) already bounds len to well under one stack frame's
 * worth of scratch space. */
I2C_UATTR static int i2c_usys_ee_write(uint16_t addr, const uint8_t *buf, int len) {
    int written = 0;
    while (written < len) {
        uint16_t curr_addr = (uint16_t)(addr + written);
        int page_offset = curr_addr % AT24C32_PAGE_SIZE;
        int chunk = AT24C32_PAGE_SIZE - page_offset;
        if (chunk > (len - written)) chunk = len - written;

        uint8_t wbuf[2 + AT24C32_PAGE_SIZE];
        wbuf[0] = (uint8_t)(curr_addr >> 8);
        wbuf[1] = (uint8_t)curr_addr;
        for (int i = 0; i < chunk; i++) wbuf[2 + i] = buf[written + i];

        if (!i2c_usys_write_bytes(AT24C32_I2C_ADDR, wbuf, 2 + chunk)) {
            return written > 0 ? written : -1;
        }
        written += chunk;
        i2c_usys_delay_us(10000);
    }
    return written;
}

I2C_UATTR static void i2c_umode_body(void) {
    /* Not a string literal: a literal lands in ordinary .rodata, outside
     * every region this task's domain grants -- the bug that hung the
     * board the first time this exact mechanism ran on real hardware in
     * M5 Phase 2 (see drivers/tm1638_rp2350.c's own comment on it).
     * volatile, for the reason kernel/shell.c's user_deputy() already
     * documents: gcc recognises a run of consecutive stores and turns it
     * back into a copy from a .rodata blob otherwise. */
    volatile char name[4];
    name[0] = 'i'; name[1] = '2'; name[2] = 'c'; name[3] = '\0';

    for (;;) {
        uint8_t req[I2C_REQ_CAP];
        long req_len = i2c_usys_chan_serve_wait((const char *)name, req, sizeof(req));
        if (req_len < 1) {
            i2c_usys_chan_serve_reply((const char *)name, NULL, 0);
            continue;
        }

        uint8_t op = req[0];
        uint8_t resp[I2C_RESP_CAP];
        uint32_t resp_len = 0;
        switch (op) {
        case I2C_OP_RTC_READ_TIME: {
            uint8_t buf[7];
            uint8_t reg = 0x00;
            bool ok = i2c_usys_read_reg(DS1307_DS3231_I2C_ADDR, &reg, 1, buf, 7);
            resp[0] = ok ? 1 : 0;
            if (ok) {
                /* Same field layout rtc_time_t's own decode uses
                 * (i2c_rtc_hw_read_time() above) -- written out field by
                 * field into the wire buffer rather than building a
                 * local rtc_time_t and copying it, so there is no
                 * struct-sized copy for the compiler to consider
                 * lowering into a call outside .utext. */
                uint8_t sec  = i2c_usys_bcd2dec(buf[0] & 0x7F);
                uint8_t min  = i2c_usys_bcd2dec(buf[1] & 0x7F);
                uint8_t hour = i2c_usys_bcd2dec(buf[2] & 0x3F);
                uint8_t day   = i2c_usys_bcd2dec(buf[4] & 0x3F);
                uint8_t month = i2c_usys_bcd2dec(buf[5] & 0x1F);
                uint16_t year = (uint16_t)(2000 + i2c_usys_bcd2dec(buf[6]));
                i2c_usys_put_u16(&resp[1], year);
                resp[3] = month; resp[4] = day; resp[5] = hour;
                resp[6] = min; resp[7] = sec;
                resp[8] = 0; resp[9] = 0; /* ms: always 0, see i2c_rtc_hw_read_time() */
                resp_len = 10;
            } else {
                resp_len = 1;
            }
            break;
        }
        case I2C_OP_RTC_WRITE_TIME: {
            bool ok = false;
            if (req_len >= 10) {
                uint16_t year = i2c_usys_get_u16(&req[1]);
                uint8_t reg_buf[8];
                reg_buf[0] = 0x00;
                reg_buf[1] = i2c_usys_dec2bcd(req[7]); /* sec */
                reg_buf[2] = i2c_usys_dec2bcd(req[6]); /* min */
                reg_buf[3] = i2c_usys_dec2bcd(req[5]); /* hour */
                reg_buf[4] = 1; /* day of week default */
                reg_buf[5] = i2c_usys_dec2bcd(req[4]); /* day */
                reg_buf[6] = i2c_usys_dec2bcd(req[3]); /* month */
                reg_buf[7] = i2c_usys_dec2bcd((uint8_t)(year >= 2000 ? (year - 2000) : year));
                ok = i2c_usys_write_bytes(DS1307_DS3231_I2C_ADDR, reg_buf, 8);
            }
            resp[0] = ok ? 1 : 0;
            resp_len = 1;
            break;
        }
        case I2C_OP_RTC_READ_TEMP: {
            uint8_t buf[2];
            uint8_t reg = 0x11;
            bool ok = i2c_usys_read_reg(DS1307_DS3231_I2C_ADDR, &reg, 1, buf, 2);
            resp[0] = ok ? 1 : 0;
            if (ok) {
                int temp_x4 = (int)(int8_t)buf[0] * 4 + (buf[1] >> 6);
                int temp_c = (temp_x4 >= 0) ? (temp_x4 + 2) / 4 : (temp_x4 - 2) / 4;
                i2c_usys_put_i32(&resp[1], (int32_t)temp_c);
                resp_len = 5;
            } else {
                resp_len = 1;
            }
            break;
        }
        case I2C_OP_EE_READ: {
            int32_t result = -1;
            if (req_len >= 5) {
                uint16_t addr = i2c_usys_get_u16(&req[1]);
                uint16_t len  = i2c_usys_get_u16(&req[3]);
                if (len <= AT24C32_CHUNK_MAX) {
                    uint8_t reg[2] = { (uint8_t)(addr >> 8), (uint8_t)addr };
                    if (i2c_usys_read_reg(AT24C32_I2C_ADDR, reg, 2, &resp[4], len)) {
                        result = (int32_t)len;
                    }
                }
            }
            i2c_usys_put_i32(&resp[0], result);
            resp_len = (result > 0) ? 4u + (uint32_t)result : 4u;
            break;
        }
        case I2C_OP_EE_WRITE: {
            int32_t result = -1;
            if (req_len >= 5) {
                uint16_t addr = i2c_usys_get_u16(&req[1]);
                uint16_t len  = i2c_usys_get_u16(&req[3]);
                if (len <= AT24C32_CHUNK_MAX && (uint32_t)req_len >= 5u + (uint32_t)len) {
                    result = (int32_t)i2c_usys_ee_write(addr, &req[5], (int)len);
                }
            }
            i2c_usys_put_i32(&resp[0], result);
            resp_len = 4;
            break;
        }
        default:
            resp_len = 0;
            break;
        }
        i2c_usys_chan_serve_reply((const char *)name, resp, resp_len);
    }
}

/* M5 heap-reclaim, plan/phase12_microkernel_migration.md: 512 bytes, not
 * 4096 -- i2c_umode_body()'s deepest call chain (through
 * i2c_usys_read_reg -> i2c_usys_target/i2c_usys_write_raw) measures 384
 * bytes on the real disassembly. See drivers/uart_rp2350.c's
 * g_heartbeat_ustack comment and .ustacks512's in linker/rp2350.ld. */
static uint8_t      g_i2c_ustack[512] __attribute__((aligned(512)))
                                       __attribute__((section(".ustacks512")));
static mem_domain_t g_i2c_domain;


/* This task's own kernel-mode entry point: task_create_sized() calls this
 * (ordinary kernel stack, kernel privilege) to build the domain and make
 * the one-way jump into U-mode. Mirrors drivers/tm1638_rp2350.c's
 * tm1638_task_body() shape: the same 3-region domain (own stack, the
 * shared .utext page, one MMIO window -- I2C_RTC_BASE's controller
 * registers here instead of SIO), the same refuse-rather-than-claim-
 * unverified-isolation rule. */
static void i2c_task_body(void *arg) {
    (void)arg;
    while (!g_i2c_ep) sched_yield();

    mem_domain_init(&g_i2c_domain);
    mem_domain_add(&g_i2c_domain, (uintptr_t)g_i2c_ustack, sizeof(g_i2c_ustack),
                   MEM_R | MEM_W);

    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&g_i2c_domain, tbase, tsize, MEM_R | MEM_X);

    mem_domain_add(&g_i2c_domain, I2C_RTC_BASE, 4096, MEM_R | MEM_W);

    if (task_set_domain(sched_current_pid(), &g_i2c_domain) != 0) {
        printk("[I2C] Refusing to enter U-mode: memory domain not enforceable; RTC/EEPROM stay on direct hardware access.\n");
        return;
    }
    arch_enter_user(i2c_umode_body, (uintptr_t)g_i2c_ustack + sizeof(g_i2c_ustack), 0, 0, 0);
}

#else /* !CONFIG_BOARD_RP2350: plain kernel-mode server, as every M4.5
       * driver task had it before M5 -- no real I2C hardware to isolate
       * on QEMU, so no reason to build a domain for it. */

/* This task, and only this task, may call the *_hw_* functions in this file
 * and drivers/at24c32.c while alive -- see uart_16550.c's uart_task_body()
 * for the fuller reasoning (never call back into anything that could
 * chan_call() this same endpoint; never take printk_lock() from here). */
static void i2c_task_body(void *arg) {
    (void)arg;
    while (!g_i2c_ep) sched_yield();

    for (;;) {
        uint32_t req_len = chan_serve_wait(g_i2c_ep);
        if (req_len < 1) { chan_serve_reply(g_i2c_ep, 0); continue; }

        uint8_t op = g_i2c_req[0];
        switch (op) {
        case I2C_OP_RTC_READ_TIME: {
            rtc_time_t tm;
            bool ok = i2c_rtc_hw_read_time(&tm);
            g_i2c_resp[0] = ok ? 1 : 0;
            if (ok) rtc_to_wire(&tm, &g_i2c_resp[1]);
            chan_serve_reply(g_i2c_ep, ok ? 1u + RTC_WIRE_LEN : 1u);
            break;
        }
        case I2C_OP_RTC_WRITE_TIME: {
            bool ok = false;
            if (req_len >= 1 + RTC_WIRE_LEN) {
                rtc_time_t tm;
                rtc_from_wire(&g_i2c_req[1], &tm);
                ok = i2c_rtc_hw_write_time(&tm);
            }
            g_i2c_resp[0] = ok ? 1 : 0;
            chan_serve_reply(g_i2c_ep, 1);
            break;
        }
        case I2C_OP_RTC_READ_TEMP: {
            int temp_c = 0;
            bool ok = i2c_rtc_hw_read_temperature_c(&temp_c);
            g_i2c_resp[0] = ok ? 1 : 0;
            {
                int32_t v = (int32_t)temp_c;
                memcpy(&g_i2c_resp[1], &v, sizeof(v)); /* native byte order -- matches get_i32() */
            }
            chan_serve_reply(g_i2c_ep, ok ? 5u : 1u);
            break;
        }
        case I2C_OP_EE_READ: {
            int32_t result = -1;
            if (req_len >= 5) {
                uint16_t addr = ((uint16_t)g_i2c_req[1] << 8) | g_i2c_req[2];
                uint16_t len  = ((uint16_t)g_i2c_req[3] << 8) | g_i2c_req[4];
                if (len <= AT24C32_CHUNK_MAX) {
                    result = (int32_t)at24c32_hw_read(addr, &g_i2c_resp[4], len);
                }
            }
            memcpy(&g_i2c_resp[0], &result, sizeof(result)); /* native byte order -- matches get_i32() */
            chan_serve_reply(g_i2c_ep, (result > 0) ? 4u + (uint32_t)result : 4u);
            break;
        }
        case I2C_OP_EE_WRITE: {
            int32_t result = -1;
            if (req_len >= 5) {
                uint16_t addr = ((uint16_t)g_i2c_req[1] << 8) | g_i2c_req[2];
                uint16_t len  = ((uint16_t)g_i2c_req[3] << 8) | g_i2c_req[4];
                if (len <= AT24C32_CHUNK_MAX && req_len >= 5u + len) {
                    result = (int32_t)at24c32_hw_write(addr, &g_i2c_req[5], len);
                }
            }
            memcpy(&g_i2c_resp[0], &result, sizeof(result)); /* native byte order -- matches get_i32() */
            chan_serve_reply(g_i2c_ep, 4);
            break;
        }
        default:
            chan_serve_reply(g_i2c_ep, 0);
            break;
        }
    }
}

#endif /* CONFIG_BOARD_RP2350 */


/* Called from kernel/main.c, after sched_init(). Not fatal if it fails:
 * i2c_rtc_read_time()/write_time()/read_temperature_c() and
 * at24c32_read()/write() all fall back to direct hardware access whenever
 * the task is not alive, same as every boot-time read before this ever
 * ran. */
int i2c_task_start(void) {
    int pid = task_create_sized("i2c", i2c_task_body, NULL, 1);
    if (pid < 0) {
        printk("[I2C] Could not start the i2c task; RTC/EEPROM stay on direct hardware access.\n");
        return -1;
    }
    if (chan_register_task("i2c", pid, g_i2c_req, sizeof(g_i2c_req),
                           g_i2c_resp, sizeof(g_i2c_resp)) != 0) {
        printk("[I2C] Could not register the i2c channel endpoint; falling back to direct hardware access.\n");
        return -1;
    }
    g_i2c_ep = chan_lookup("i2c");
    g_i2c_task_pid = pid;
    printk("[I2C] Driver running as task #%d, reachable via chan_call(\"i2c\", ...)\n", pid);
    return pid;
}

int i2c_task_call(const uint8_t *req, uint32_t req_len, uint8_t *resp, uint32_t resp_max) {
    for (int attempt = 0; attempt < 8; attempt++) {
        int n = chan_call(g_i2c_ep, req, req_len, resp, resp_max);
        /* M5 Phase 3: counted here, on the client side -- see
         * drivers/tm1638_rp2350.c's tm1638_call_with_retry() comment,
         * same reasoning: a U-mode server cannot touch g_i2c_calls, an
         * ordinary kernel .bss global no domain grants it. */
        if (n >= 0) { g_i2c_calls++; return n; }
        sched_yield();
    }
    return -1;
}

#if defined(CONFIG_BOARD_RP2350)
/* M5 Phase 3's own "Verify" deliverable: does the real i2c domain shape
 * (stack + .utext + a 4096-byte I2C_RTC_BASE window) actually confine the
 * task to the I2C controller, or does the grant's width accidentally
 * cover more? Modeled directly on drivers/tm1638_rp2350.c's
 * tm1638_isolation_test() -- same idea (a deliberate out-of-domain
 * store, asserted to fault), a separate canary rather than reaching into
 * another file's, for the same reason the syscall stubs above are
 * hand-rolled per file. Only meaningful where the "i2c" task actually runs
 * in U-mode -- see this file's own #if defined(CONFIG_BOARD_RP2350) split
 * above. */
static volatile uintptr_t g_i2c_canary = 0xC0FFEE;

I2C_UATTR static void i2c_intruder(void) {
    g_i2c_canary = 0xDEAD;
    for (;;) { } /* only reached if the store was NOT stopped */
}

static volatile bool g_i2c_intruder_entered;

/* `arg` is the U-mode stack -- allocated by i2c_isolation_test() below,
 * not here, so it can free it once the task is confirmed DEAD (same
 * shape as tm1638_isolation_test()'s own probe). */
static void i2c_intruder_task_body(void *arg) {
    uint8_t *ustack = (uint8_t *)arg;
    mem_domain_t dom;
    mem_domain_init(&dom);
    mem_domain_add(&dom, (uintptr_t)ustack, 4096, MEM_R | MEM_W);
    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&dom, tbase, tsize, MEM_R | MEM_X);
    /* The exact grant real i2c runs under -- this is what's on trial. */
    mem_domain_add(&dom, I2C_RTC_BASE, 4096, MEM_R | MEM_W);

    if (task_set_domain(sched_current_pid(), &dom) != 0) {
        printk("[I2CIso] Refusing to enter U-mode: memory domain not enforceable\n");
        return;
    }
    g_i2c_intruder_entered = true;
    arch_enter_user(i2c_intruder, (uintptr_t)ustack + 4096, 0, 0, 0);
}

bool i2c_isolation_test(uintptr_t *out_canary, bool *out_exited_clean) {
    g_i2c_canary = 0xC0FFEE;
    g_i2c_intruder_entered = false;

    void *ustack = palloc_pages(1);
    if (!ustack) {
        *out_canary = g_i2c_canary;
        *out_exited_clean = true;
        return false;
    }

    int pid = task_create("i2c_intruder", i2c_intruder_task_body, ustack);
    if (pid < 0) {
        palloc_free(ustack, 1);
        *out_canary = g_i2c_canary;
        *out_exited_clean = true;
        return false;
    }
    for (int i = 0; i < 10000 && sched_task_state(pid) != TASK_DEAD; i++) {
        sched_yield();
    }
    long status;
    *out_exited_clean = sched_task_exited_cleanly(pid, &status);
    *out_canary = g_i2c_canary;
    palloc_free(ustack, 1);
    return g_i2c_intruder_entered;
}
#endif /* CONFIG_BOARD_RP2350 */

bool i2c_rtc_read_time(rtc_time_t *tm) {
    if (!tm) return false;
    if (i2c_task_alive()) {
        uint8_t req[1] = { I2C_OP_RTC_READ_TIME };
        uint8_t resp[1 + RTC_WIRE_LEN];
        int n = i2c_task_call(req, sizeof(req), resp, sizeof(resp));
        if (n >= 1) {
            if (resp[0] == 0) return false;
            if ((uint32_t)n >= 1 + RTC_WIRE_LEN) {
                rtc_from_wire(&resp[1], tm);
                return true;
            }
        }
        /* IPC failed -- fall through to direct access. */
    }
    return i2c_rtc_hw_read_time(tm);
}

bool i2c_rtc_write_time(const rtc_time_t *tm) {
    if (!tm) return false;
    if (i2c_task_alive()) {
        uint8_t req[1 + RTC_WIRE_LEN];
        req[0] = I2C_OP_RTC_WRITE_TIME;
        rtc_to_wire(tm, &req[1]);
        uint8_t resp[1];
        int n = i2c_task_call(req, sizeof(req), resp, sizeof(resp));
        if (n >= 1) return resp[0] != 0;
        /* IPC failed -- fall through to direct access. */
    }
    return i2c_rtc_hw_write_time(tm);
}

bool i2c_rtc_read_temperature_c(int *temp_c) {
    if (!temp_c) return false;
    if (i2c_task_alive()) {
        uint8_t req[1] = { I2C_OP_RTC_READ_TEMP };
        uint8_t resp[5];
        int n = i2c_task_call(req, sizeof(req), resp, sizeof(resp));
        if (n >= 1) {
            if (resp[0] == 0) return false;
            if ((uint32_t)n >= 5) {
                int32_t v;
                memcpy(&v, &resp[1], sizeof(v));
                *temp_c = (int)v;
                return true;
            }
        }
        /* IPC failed -- fall through to direct access. */
    }
    return i2c_rtc_hw_read_temperature_c(temp_c);
}
