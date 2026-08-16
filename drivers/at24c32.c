#include "drivers/at24c32.h"
#include "drivers/i2c_rtc.h"
#include "lugalos_config.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include <string.h>

static bool g_at24c32_detected = false;

#if defined(CONFIG_BOARD_RP2350)
/* M5 Phase 3, plan/phase12_microkernel_migration.md: was hardcoded to
 * I2C0's base regardless of board persona -- harmless only because
 * rp2350-clock (I2C1, cmake/board-rp2350-clock.cmake) has no EEPROM
 * wired, so every access here against the wrong peripheral just failed
 * cleanly (reported as "no EEPROM detected"), not because it was
 * correct. drivers/i2c_rtc.c's own I2C_RTC_BASE already reads this
 * correctly; RTC and EEPROM share one physical bus (drivers/i2c_rtc.h),
 * so they must always agree on which peripheral that is. Found while
 * this file's own hardware access was being rewritten for U-mode
 * anyway, fixed here rather than left next to code that just got it
 * right. */
#define I2C_EE_BASE             ((uintptr_t)CONFIG_I2C_RTC_BASE)
#define IC_DATA_CMD            (I2C_EE_BASE + 0x10)
#define IC_CLR_TX_ABRT         (I2C_EE_BASE + 0x54)
#define IC_ENABLE              (I2C_EE_BASE + 0x6C)
#define IC_STATUS              (I2C_EE_BASE + 0x70)
#define IC_RAW_INTR_STAT       (I2C_EE_BASE + 0x34)
#define IC_TAR                 (I2C_EE_BASE + 0x04)

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

int at24c32_hw_read(uint16_t addr, uint8_t *buf, size_t len) {
    if (addr >= AT24C32_SIZE_BYTES) return 0;
    if (addr + len > AT24C32_SIZE_BYTES) len = AT24C32_SIZE_BYTES - addr;
    if (len == 0) return 0;

    if (i2c_read_at24(addr, buf, len)) {
        return (int)len;
    }
    return -1;
}

int at24c32_hw_write(uint16_t addr, const uint8_t *buf, size_t len) {
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

/* M4.5, plan/phase12_microkernel_migration.md, Part B: public facades routed
 * through the shared "i2c" driver task (drivers/i2c_rtc.c owns it -- see
 * drivers/i2c_rtc.h's comment on why RTC and EEPROM share one task) whenever
 * it is alive, falling back to at24c32_hw_read()/write() above otherwise --
 * same fallback shape as every other M4.5 driver-task conversion. The wire
 * request carries addr/len as plain uint16_t and the response as a plain
 * int32_t result: this call never leaves the kernel's own address space (no
 * real wire, unlike the SD card's SPI protocol or a 9P message), so there is
 * nothing for an explicit byte-order encoding to protect against here. */
#define I2C_EE_OP_READ  ((uint8_t)'R')
#define I2C_EE_OP_WRITE ((uint8_t)'X')

/* M5 Phase 3, plan/phase12_microkernel_migration.md: capped at 128 bytes
 * per wire message (was AT24C32_SIZE_BYTES, ~4 KB) -- far more than a
 * syscall-sized buffer inside a U-mode task should carry, and the reason
 * i2c was deferred when tm1638 converted to U-mode in Phase 2.
 * at24c32_read()/at24c32_write() below loop internally for anything
 * larger, so this is invisible to every existing caller (user/lisp/
 * lisp.c's (eeprom-read)/(eeprom-write), fs/vfs_server.c's EEPROM device
 * node) -- none of them need to change. */
#define I2C_EE_REQ_CAP  (5u + AT24C32_CHUNK_MAX)
#define I2C_EE_RESP_CAP (4u + AT24C32_CHUNK_MAX)

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static int32_t get_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof(v)); return v; }

/* One wire round trip, at most AT24C32_CHUNK_MAX bytes -- the chunking
 * loops below call this repeatedly for anything larger. */
static int at24c32_read_chunk(uint16_t addr, uint8_t *buf, uint16_t len) {
    uint8_t req[5];
    req[0] = I2C_EE_OP_READ;
    put_u16(&req[1], addr);
    put_u16(&req[3], len);
    uint8_t resp[I2C_EE_RESP_CAP];
    int n = i2c_task_call(req, sizeof(req), resp, sizeof(resp));
    if (n < 4) return -1;
    int32_t result = get_i32(resp);
    if (result < 0) return -1;
    if ((uint32_t)n < 4u + (uint32_t)result) return -1;
    if (result > 0) memcpy(buf, &resp[4], (size_t)result);
    return result;
}

static int at24c32_write_chunk(uint16_t addr, const uint8_t *buf, uint16_t len) {
    uint8_t req[I2C_EE_REQ_CAP];
    req[0] = I2C_EE_OP_WRITE;
    put_u16(&req[1], addr);
    put_u16(&req[3], len);
    memcpy(&req[5], buf, len);
    uint8_t resp[4];
    int n = i2c_task_call(req, 5u + (uint32_t)len, resp, sizeof(resp));
    if (n < 4) return -1;
    return (int)get_i32(resp);
}

int at24c32_read(uint16_t addr, uint8_t *buf, size_t len) {
    size_t got = 0;
    if (len <= AT24C32_SIZE_BYTES && i2c_task_alive()) {
        while (got < len) {
            uint16_t chunk = (len - got) > AT24C32_CHUNK_MAX
                                 ? (uint16_t)AT24C32_CHUNK_MAX : (uint16_t)(len - got);
            int n = at24c32_read_chunk((uint16_t)(addr + got), &buf[got], chunk);
            if (n < 0) break; /* IPC failed -- fall through to direct access below, same as every other M4.5 facade */
            got += (size_t)n;
            /* Short read: the device gave back less than asked, so more
             * would only repeat the same shortfall -- stop rather than
             * loop on it. */
            if ((uint16_t)n < chunk) return (int)got;
        }
        if (got >= len) return (int)got;
    }
    /* Covers both "task never alive" (got == 0) and "IPC failed partway
     * through the chunking loop" (got > 0) -- direct access picks up
     * wherever IPC left off, rather than abandoning bytes IPC had already
     * fetched. */
    int n = at24c32_hw_read((uint16_t)(addr + got), &buf[got], len - got);
    return n >= 0 ? (int)(got + (size_t)n) : (got > 0 ? (int)got : -1);
}

int at24c32_write(uint16_t addr, const uint8_t *buf, size_t len) {
    size_t written = 0;
    if (len <= AT24C32_SIZE_BYTES && i2c_task_alive()) {
        while (written < len) {
            uint16_t chunk = (len - written) > AT24C32_CHUNK_MAX
                                  ? (uint16_t)AT24C32_CHUNK_MAX : (uint16_t)(len - written);
            int n = at24c32_write_chunk((uint16_t)(addr + written), &buf[written], chunk);
            if (n < 0) break;
            written += (size_t)n;
            if ((uint16_t)n < chunk) return (int)written;
        }
        if (written >= len) return (int)written;
    }
    int n = at24c32_hw_write((uint16_t)(addr + written), &buf[written], len - written);
    return n >= 0 ? (int)(written + (size_t)n) : (written > 0 ? (int)written : -1);
}
