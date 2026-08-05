#include "drivers/i2c_rtc.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "drivers/uart.h"
#include <string.h>

#define DS1307_DS3231_I2C_ADDR 0x68

static bool g_rtc_detected = false;
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5

#if defined(CONFIG_BOARD_RP2350)
#define RESETS_BASE            0x40020000UL
#define RESETS_RESET_CLR       (RESETS_BASE + 0x008)
#define RESETS_RESET_DONE      (RESETS_BASE + 0x00C)
#define I2C0_RESET_BIT         (1u << 4)

#define IO_BANK0_BASE          0x40028000UL
#define IO_BANK0_CTRL(n)       (IO_BANK0_BASE + 0x004 + (n) * 8)

#define PADS_BANK0_BASE        0x40038000UL
#define PADS_BANK0_PAD(n)      (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define I2C0_BASE              0x40090000UL
#define IC_CON                 (I2C0_BASE + 0x00)
#define IC_TAR                 (I2C0_BASE + 0x04)
#define IC_DATA_CMD            (I2C0_BASE + 0x10)
#define IC_SS_SCL_HCNT         (I2C0_BASE + 0x14)
#define IC_SS_SCL_LCNT         (I2C0_BASE + 0x18)
#define IC_FS_SCL_HCNT         (I2C0_BASE + 0x1C)
#define IC_FS_SCL_LCNT         (I2C0_BASE + 0x20)
#define IC_INTR_STAT           (I2C0_BASE + 0x2C)
#define IC_RAW_INTR_STAT       (I2C0_BASE + 0x34)
#define IC_CLR_TX_ABRT         (I2C0_BASE + 0x54)
#define IC_ENABLE              (I2C0_BASE + 0x6C)
#define IC_STATUS              (I2C0_BASE + 0x70)
#define IC_TXFLR               (I2C0_BASE + 0x74)
#define IC_RXFLR               (I2C0_BASE + 0x78)
#define IC_TX_ABRT_SOURCE      (I2C0_BASE + 0x80)

#define REG(addr) (*(volatile uint32_t *)(addr))

static void i2c_hw_init(void) {
    /* 1. Unreset I2C0 */
    REG(RESETS_RESET_CLR) = I2C0_RESET_BIT;
    int timeout = 10000;
    while (!(REG(RESETS_RESET_DONE) & I2C0_RESET_BIT) && --timeout > 0);

    /* Configure GP4 (SDA) & GP5 (SCL) strictly as Function 3 (I2C) */
    REG(IO_BANK0_CTRL(I2C_SDA_PIN)) = 3;
    REG(IO_BANK0_CTRL(I2C_SCL_PIN)) = 3;

    /* Enable Pull-ups on GP4 & GP5 pads (0x5A) */
    REG(PADS_BANK0_PAD(I2C_SDA_PIN)) = 0x5A;
    REG(PADS_BANK0_PAD(I2C_SCL_PIN)) = 0x5A;

    /* Disable I2C0 before configuring */
    REG(IC_ENABLE) = 0;

    /* Master mode (bit 0), 7-bit addressing, Fast mode (400kHz, bit 2), Restart enable (bit 6), Slave disable (bit 5), TX_EMPTY_CTRL (bit 8) */
    REG(IC_CON) = (1u << 0) | (1u << 5) | (2u << 1) | (1u << 6) | (1u << 8);
    REG(IC_TAR) = DS1307_DS3231_I2C_ADDR;

    /* Fast Mode Clock Dividers for 150MHz system clock */
    REG(IC_SS_SCL_HCNT) = 750;
    REG(IC_SS_SCL_LCNT) = 750;
    REG(IC_FS_SCL_HCNT) = 150;
    REG(IC_FS_SCL_LCNT) = 225;

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
    uint8_t dummy = 0x00;
    return i2c_write_bytes(addr, &dummy, 1);
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
            printk("[I2C RTC] DS1307/DS3231 detected at 0x68 (GP4/GP5)! Synced date: %s\n", isostr);
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

void i2c_scan_bus(void) {
    printk("\nI2C Bus Scan (I2C0 on GP4 SDA / GP5 SCL):\n");
    printk("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

    int found_count = 0;
    for (int row = 0; row < 128; row += 16) {
        printk("%02x: ", row);
        for (int col = 0; col < 16; col++) {
            uint8_t addr = row + col;
            if (addr < 0x08 || addr > 0x77) {
                printk("   ");
            } else {
                if (i2c_probe_addr(addr)) {
                    printk("%02x ", addr);
                    found_count++;
                } else {
                    printk("-- ");
                }
            }
        }
        printk("\n");
    }
    printk("Found %d I2C device(s).\n\n", found_count);
}
