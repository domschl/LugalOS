#include "drivers/at24c32.h"
#include "drivers/i2c_rtc.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include <string.h>

static bool g_at24c32_detected = false;

#if defined(CONFIG_BOARD_RP2350)
#define I2C0_BASE              0x40090000UL
#define IC_DATA_CMD            (I2C0_BASE + 0x10)
#define IC_CLR_TX_ABRT         (I2C0_BASE + 0x54)
#define IC_ENABLE              (I2C0_BASE + 0x6C)
#define IC_STATUS              (I2C0_BASE + 0x70)
#define IC_RAW_INTR_STAT       (I2C0_BASE + 0x34)
#define IC_TAR                 (I2C0_BASE + 0x04)

#define REG(addr) (*(volatile uint32_t *)(addr))

static bool i2c_write_at24(uint16_t mem_addr, const uint8_t *src, size_t len) {
    REG(IC_ENABLE) = 0;
    REG(IC_TAR) = AT24C32_I2C_ADDR;
    REG(IC_ENABLE) = 1;
    (void)REG(IC_CLR_TX_ABRT);

    // Send 16-bit address (high byte, low byte)
    uint8_t addr_bytes[2] = { (uint8_t)(mem_addr >> 8), (uint8_t)(mem_addr & 0xFF) };

    bool abort = false;
    for (int i = 0; i < 2; i++) {
        int timeout = 10000;
        while (!(REG(IC_STATUS) & (1u << 1)) && --timeout > 0);
        if (timeout == 0) return false;

        REG(IC_DATA_CMD) = addr_bytes[i];

        timeout = 10000;
        do {
            if (REG(IC_RAW_INTR_STAT) & (1u << 6)) {
                abort = true;
                (void)REG(IC_CLR_TX_ABRT);
                break;
            }
        } while (--timeout > 0 && !(REG(IC_RAW_INTR_STAT) & (1u << 4)));

        if (abort || timeout == 0) return false;
    }

    // Send payload data bytes
    for (size_t i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = src[i];
        if (last) cmd |= (1u << 9); // STOP bit

        int timeout = 10000;
        while (!(REG(IC_STATUS) & (1u << 1)) && --timeout > 0);
        if (timeout == 0) return false;

        REG(IC_DATA_CMD) = cmd;

        timeout = 10000;
        do {
            if (REG(IC_RAW_INTR_STAT) & (1u << 6)) {
                abort = true;
                (void)REG(IC_CLR_TX_ABRT);
                break;
            }
        } while (--timeout > 0 && !(REG(IC_RAW_INTR_STAT) & (1u << 4)));

        if (abort || timeout == 0) return false;
    }
    return !abort;
}

static bool i2c_read_at24(uint16_t mem_addr, uint8_t *dst, size_t len) {
    REG(IC_ENABLE) = 0;
    REG(IC_TAR) = AT24C32_I2C_ADDR;
    REG(IC_ENABLE) = 1;
    (void)REG(IC_CLR_TX_ABRT);

    // 1. Write 16-bit memory address
    uint8_t addr_bytes[2] = { (uint8_t)(mem_addr >> 8), (uint8_t)(mem_addr & 0xFF) };
    bool abort = false;
    for (int i = 0; i < 2; i++) {
        int timeout = 10000;
        while (!(REG(IC_STATUS) & (1u << 1)) && --timeout > 0);
        if (timeout == 0) return false;

        REG(IC_DATA_CMD) = addr_bytes[i];

        timeout = 10000;
        do {
            if (REG(IC_RAW_INTR_STAT) & (1u << 6)) {
                abort = true;
                (void)REG(IC_CLR_TX_ABRT);
                break;
            }
        } while (--timeout > 0 && !(REG(IC_RAW_INTR_STAT) & (1u << 4)));

        if (abort || timeout == 0) return false;
    }

    // 2. Read bytes
    REG(IC_ENABLE) = 0;
    REG(IC_TAR) = AT24C32_I2C_ADDR;
    REG(IC_ENABLE) = 1;
    (void)REG(IC_CLR_TX_ABRT);

    for (size_t i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t cmd = (1u << 8); // READ bit
        if (last) cmd |= (1u << 9); // STOP bit

        int timeout = 10000;
        while (!(REG(IC_STATUS) & (1u << 1)) && --timeout > 0);
        if (timeout == 0) return false;

        REG(IC_DATA_CMD) = cmd;

        timeout = 10000;
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
    return !abort;
}
#else
// Synthetic RAM EEPROM for QEMU targets
static uint8_t g_qemu_eeprom[AT24C32_SIZE_BYTES];
static bool i2c_write_at24(uint16_t mem_addr, const uint8_t *src, size_t len) {
    if (mem_addr + len > AT24C32_SIZE_BYTES) return false;
    memcpy(&g_qemu_eeprom[mem_addr], src, len);
    return true;
}
static bool i2c_read_at24(uint16_t mem_addr, uint8_t *dst, size_t len) {
    if (mem_addr + len > AT24C32_SIZE_BYTES) return false;
    memcpy(dst, &g_qemu_eeprom[mem_addr], len);
    return true;
}
#endif

void at24c32_init(void) {
    uint8_t dummy = 0;
    g_at24c32_detected = false;

    if (i2c_read_at24(0x0000, &dummy, 1)) {
        g_at24c32_detected = true;
        printk("[AT24C32] 4KB I2C EEPROM detected at 0x57!\n");
    } else {
        printk("[AT24C32] No EEPROM detected at 0x57 (Using synthetic 4KB RAM buffer).\n");
    }
}

bool at24c32_is_detected(void) {
    return g_at24c32_detected;
}

int at24c32_read(uint16_t addr, uint8_t *buf, size_t len) {
    if (addr >= AT24C32_SIZE_BYTES) return 0;
    if (addr + len > AT24C32_SIZE_BYTES) len = AT24C32_SIZE_BYTES - addr;
    if (len == 0) return 0;

    if (i2c_read_at24(addr, buf, len)) {
        return (int)len;
    }
    return -1;
}

int at24c32_write(uint16_t addr, const uint8_t *buf, size_t len) {
    if (addr >= AT24C32_SIZE_BYTES) return 0;
    if (addr + len > AT24C32_SIZE_BYTES) len = AT24C32_SIZE_BYTES - addr;
    if (len == 0) return 0;

    size_t written = 0;
    while (written < len) {
        uint16_t curr_addr = addr + written;
        size_t page_offset = curr_addr % AT24C32_PAGE_SIZE;
        size_t chunk = AT24C32_PAGE_SIZE - page_offset;
        if (chunk > (len - written)) chunk = len - written;

        if (!i2c_write_at24(curr_addr, &buf[written], chunk)) {
            return written > 0 ? (int)written : -1;
        }
        written += chunk;
        time_delay_us(10000); // Wait 10ms EEPROM internal page write cycle
    }
    return (int)written;
}
