#include "drivers/i2c_rtc.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
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

void i2c_rtc_init(void) {
    i2c_hw_init();

    rtc_time_t tm;
    g_rtc_detected = false;

    if (i2c_rtc_read_time(&tm)) {
        if (tm.month >= 1 && tm.month <= 12 && tm.day >= 1 && tm.day <= 31 && tm.hour <= 23 && tm.min <= 59 && tm.sec <= 59) {
            g_rtc_detected = true;
            time_set_rtc(&tm);
            char isostr[32];
            time_format_iso(&tm, isostr, sizeof(isostr));
#if defined(CONFIG_BOARD_RP2350)
            printk("[I2C RTC] DS1307/DS3231 detected at 0x68 (GP%d/GP%d)! Synced date: %s\n",
                   CONFIG_I2C_RTC_SDA_GPIO, CONFIG_I2C_RTC_SCL_GPIO, isostr);
#else
            printk("[I2C RTC] DS1307/DS3231 detected at 0x68! Synced date: %s\n", isostr);
#endif
            return;
        }
    }

    printk("[I2C RTC] No DS1307/DS3231 RTC module found at 0x68 (Using system software clock).\n");
}

bool i2c_rtc_is_detected(void) {
    return g_rtc_detected;
}

bool i2c_rtc_read_time(rtc_time_t *tm) {
    if (!tm) return false;
    uint8_t buf[7];
    if (!i2c_read_bytes(DS1307_DS3231_I2C_ADDR, 0x00, buf, 7)) {
        return false;
    }

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

bool i2c_rtc_write_time(const rtc_time_t *tm) {
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

    if (g_rtc_detected) {
        return i2c_write_bytes(DS1307_DS3231_I2C_ADDR, reg_buf, 8);
    }
    return false;
}

bool i2c_rtc_read_temperature_c(int *temp_c) {
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
