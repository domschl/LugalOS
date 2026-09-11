#include "drivers/at24c32.h"
#include "drivers/i2c_bus.h"
#include "lugalos_config.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include <string.h>

static bool g_at24c32_detected = false;

/* --- Bus access (phase 30 category E) ---------------------------------
 *
 * Every transfer goes through i2c_xfer(), the one generic operation the
 * shared bus offers. This file used to carry **its own copy of the RP2350
 * controller registers** -- IC_DATA_CMD, IC_TAR, IC_ENABLE, IC_STATUS -- for
 * the same peripheral drivers/i2c_rtc.c already drove, which is two
 * implementations of one controller in two files, and the older of them is
 * the one that still puts a STOP where a repeated START belongs
 * (plan/open_issues.md). Deleting it is the point: a fix to the transfer
 * shape now lands once.
 *
 * The synthetic buffer stays for targets with no controller at all -- QEMU,
 * where /dev/eeprom, the eeprom-read/write primitives and the identity store
 * are all exercised against it. What decides between them is
 * I2C_HAVE_CONTROLLER (drivers/i2c_rtc.h), not a board name. */
#if I2C_HAVE_CONTROLLER

/* Reads are chunked to what one transfer carries; writes are already chunked
 * to the part's 32-byte page by at24c32_hw_write() below, which is a smaller
 * bound and for a different reason (a page write that crosses a boundary
 * wraps inside the part rather than continuing). */
static bool i2c_read_at24(uint16_t mem_addr, uint8_t *dst, size_t len) {
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > I2C_XFER_RMAX) chunk = I2C_XFER_RMAX;
        uint16_t a = (uint16_t)(mem_addr + done);
        uint8_t addr_bytes[2] = { (uint8_t)(a >> 8), (uint8_t)(a & 0xFF) };
        if (!i2c_xfer(AT24C32_I2C_ADDR, addr_bytes, 2, &dst[done], (uint32_t)chunk))
            return false;
        done += chunk;
    }
    return true;
}

static bool i2c_write_at24(uint16_t mem_addr, const uint8_t *src, size_t len) {
    if (len + 2u > I2C_XFER_WMAX) return false;
    uint8_t buf[I2C_XFER_WMAX];
    buf[0] = (uint8_t)(mem_addr >> 8);
    buf[1] = (uint8_t)(mem_addr & 0xFF);
    for (size_t i = 0; i < len; i++) buf[2 + i] = src[i];
    return i2c_xfer(AT24C32_I2C_ADDR, buf, (uint32_t)(len + 2u), NULL, 0);
}

#else
// Synthetic RAM EEPROM for targets with no I2C controller
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
#if I2C_HAVE_CONTROLLER
    /* A real probe now, on every board that has a bus, rather than only on
     * RP2350 (phase 30 category E). The ESP32-P4 has an I2C controller and
     * announced a synthetic buffer regardless, so an EEPROM wired to it was
     * simply never used. */
    uint8_t dummy = 0;
    g_at24c32_detected = i2c_read_at24(0x0000, &dummy, 1);
    if (g_at24c32_detected) {
        printk("[AT24C32] 4KB I2C EEPROM detected at 0x%02x.\n", AT24C32_I2C_ADDR);
    } else {
        printk("[AT24C32] No EEPROM answered at 0x%02x; the 4 KB store is "
               "unavailable on this board.\n", AT24C32_I2C_ADDR);
    }
#else
    /* No bus at all, so nothing to probe and nothing that could fail one --
     * which is how a board with no I2C controller once came to announce "4KB
     * I2C EEPROM detected at 0x57!" on its first boot (E2,
     * plan/phase27_esp32p4_bringup.md).
     *
     * `detected` stays true, and that is not a compromise: /dev/eeprom, the
     * `eeprom-read`/`eeprom-write` primitives and the identity store all work
     * against this buffer, so the device really is present and usable. It is
     * simply not a chip, and only the message was ever claiming otherwise. */
    g_at24c32_detected = true;
    printk("[AT24C32] Synthetic 4 KB RAM EEPROM (no I2C bus on this target).\n");
#endif
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

/* The public facade. Once a device is an i2c_xfer() client there is nothing
 * left for it to do (phase 30 category E).
 *
 * This used to be ninety lines: a second wire protocol ('R'/'X' opcodes,
 * addr/len marshalling, an int32 result), two chunking loops bounded by
 * AT24C32_CHUNK_MAX, and a fallback path that resumed direct access from
 * wherever IPC had failed partway. Every one of those concerns belongs to the
 * bus, not to the EEPROM, and i2c_xfer() already handles all of them --
 * routing through the "i2c" task when it is alive, going direct before it
 * exists, and bounding the transfer. The chunking that remains is the part's
 * own: 32-byte pages on write (a write that crosses a page boundary wraps
 * inside the chip), and the transfer bound on read. */
int at24c32_read(uint16_t addr, uint8_t *buf, size_t len) {
    return at24c32_hw_read(addr, buf, len);
}

int at24c32_write(uint16_t addr, const uint8_t *buf, size_t len) {
    return at24c32_hw_write(addr, buf, len);
}
