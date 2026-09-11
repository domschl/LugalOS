/*
 * LugalOS driver: SPI flash on the ESP32-P4 -- E6,
 * plan/phase27_esp32p4_bringup.md.
 *
 * ## Why this calls the boot ROM instead of driving the MSPI
 *
 * The P4's ROM exports the whole SPI flash API at fixed addresses, and this
 * file calls those. That is the same choice RP2350 makes with its bootrom
 * (arch/riscv/rp2350/bootrom.c), for the same reasons: the ROM routines are
 * what Espressif's own bootloader and esptool stub use, they already know
 * this chip's command set and timing, and the alternative -- a fresh MSPI
 * driver -- is a large amount of new code whose failure mode is a corrupted
 * flash rather than a wrong number on a console.
 *
 * The addresses are transcribed from ESP-IDF's own ROM linker script,
 * components/esp_rom/esp32p4/ld/esp32p4.rom.ld, and cross-checked against
 * esp32p4.rom.eco0_4.ld -- the revision-specific override file -- where they
 * are **identical**. That check is the point: this board is v1.3, and a ROM
 * entry point that moved between revisions would be a call into whatever
 * happens to sit at the old address.
 *
 * ## There is no XIP window here
 *
 * On RP2350 the filesystem is read by dereferencing a pointer into the XIP
 * window. Nothing like that exists on this board as this kernel runs: the
 * image is loaded into L2MEM by `esptool load-ram` and flash is not mapped
 * into the address space at all. Every read is a transfer. drivers/flashdisk.c
 * has a matching arm for exactly this reason.
 *
 * ## The write guard, which is the safety property of this milestone
 *
 * This board shipped with a Waveshare factory demo that cannot be obtained
 * again once overwritten. The partition table was read out of the verified
 * backup and parsed (cmake/flash_layout_esp32p4.cmake records the result):
 * the factory image ends at 0x00D10000 and everything above reads 0xFF. The
 * writable region starts at 0x00E00000, ~0.9 MB higher.
 *
 * Every erase and every program in this file checks that floor and refuses
 * below it. Not because a caller might get it wrong -- because a guard in the
 * one place that touches the hardware is a property of the system, while a
 * convention followed by callers is a property of whoever wrote them.
 */

#include "drivers/flash_esp32p4.h"
#include "kernel/printk.h"
#include "kernel/lock.h"
#include "lugalos_config.h"
#include <string.h>

/* --- the ROM entry points ---------------------------------------------- */

#define ROM_SPI_FLASH_ATTACH        0x4fc001e8UL  /* spi_flash_attach        */
#define ROM_SPIFLASH_ERASE_SECTOR   0x4fc0014cUL
#define ROM_SPIFLASH_WRITE          0x4fc00154UL
#define ROM_SPIFLASH_READ           0x4fc00158UL
#define ROM_SPIFLASH_UNLOCK         0x4fc0015cUL
#define ROM_SPIFLASH_CONFIG_PARAM   0x4fc00168UL

/* ESP_ROM_SPIFLASH_RESULT_OK is 0 in IDF's enum. */
#define ROM_OK 0

typedef void (*rom_attach_fn)(uint32_t ishspi, bool legacy);
typedef int  (*rom_erase_sector_fn)(uint32_t sector_num);
typedef int  (*rom_write_fn)(uint32_t dest_addr, const uint32_t *src, int32_t len);
typedef int  (*rom_read_fn)(uint32_t src_addr, uint32_t *dest, int32_t len);
typedef int  (*rom_unlock_fn)(void);
typedef int  (*rom_config_param_fn)(uint32_t deviceId, uint32_t chip_size,
                                    uint32_t block_size, uint32_t sector_size,
                                    uint32_t page_size, uint32_t status_mask);

#define rom_attach        ((rom_attach_fn)ROM_SPI_FLASH_ATTACH)
#define rom_erase_sector  ((rom_erase_sector_fn)ROM_SPIFLASH_ERASE_SECTOR)
#define rom_write         ((rom_write_fn)ROM_SPIFLASH_WRITE)
#define rom_read          ((rom_read_fn)ROM_SPIFLASH_READ)
#define rom_unlock        ((rom_unlock_fn)ROM_SPIFLASH_UNLOCK)
#define rom_config_param  ((rom_config_param_fn)ROM_SPIFLASH_CONFIG_PARAM)

/* The chip, from the backup's own record: GigaDevice, manufacturer 0xc8,
 * device 0x4018, 16 MB. Written out rather than probed because the flash map
 * was built against this size, and a chip that is not this one should be
 * reported rather than silently accommodated. */
#define FLASH_P4_DEVICE_ID   0x001840c8u
#define FLASH_P4_BLOCK_SIZE  65536u
#define FLASH_P4_PAGE_SIZE   256u
#define FLASH_P4_STATUS_MASK 0xffffu

static bool       g_ready;


/* The ROM routines are not re-entrant and a sector erase takes tens of
 * milliseconds. A ylock_t rather than a spinlock_t: the wait can be long
 * enough that spinning through it would deny the CPU to everything else, and
 * kernel/lock.h is explicit that "a spinlock_t taken where a ylock_t was
 * needed deadlocks on one hart" -- and since phase 31 Y2 it also refuses the
 * mistake at run time rather than only describing it. */
static ylock_t g_flash_ylock;

static inline bool writable(uint32_t addr, uint32_t len) {
    uint32_t end = addr + len;
    if (end < addr) return false;                      /* wrapped */
    if (addr < (uint32_t)LUGALOS_P4_FLASH_WRITABLE_FLOOR) return false;
    if (end > (uint32_t)LUGALOS_P4_FLASH_SIZE) return false;
    return true;
}

bool flash_p4_ready(void) { return g_ready; }
uint32_t flash_p4_size(void) { return g_ready ? (uint32_t)LUGALOS_P4_FLASH_SIZE : 0u; }

bool flash_p4_init(void) {
    if (g_ready) return true;

    ylock_init(&g_flash_ylock);

    /* Attach first: after `esptool load-ram` the ROM is in download mode and
     * has not necessarily configured the SPI pins for flash at all, so this
     * cannot be assumed from a normal boot's leftovers -- the same argument
     * drivers/uart_esp32p4.c makes about the console.
     *
     * ishspi = 0 selects the standard flash pins; legacy = false is what
     * IDF's own bootloader passes on this part. */
    rom_attach(0, false);

    if (rom_config_param(FLASH_P4_DEVICE_ID, (uint32_t)LUGALOS_P4_FLASH_SIZE,
                         FLASH_P4_BLOCK_SIZE, FLASH_P4_SECTOR_SIZE,
                         FLASH_P4_PAGE_SIZE, FLASH_P4_STATUS_MASK) != ROM_OK) {
        printk("[Flash] ROM refused the chip parameters; /flash0 stays unavailable\n");
        return false;
    }

    g_ready = true;
    printk("[Flash] SPI flash via boot ROM: %u MB, writable region 0x%06x..0x%06x\n",
           (unsigned)((uint32_t)LUGALOS_P4_FLASH_SIZE / (1024u * 1024u)),
           (unsigned)LUGALOS_P4_FLASHFS_BASE,
           (unsigned)((uint32_t)LUGALOS_P4_FLASHFS_BASE + (uint32_t)LUGALOS_P4_FLASHFS_SIZE));
    return true;
}

/* The ROM read takes a word pointer and a length, and both the address and
 * the destination want 4-byte alignment. Rather than push that onto callers
 * -- a block device hands out whatever the filesystem asked for -- anything
 * unaligned is routed through a small bounce buffer here. */
int flash_p4_read(uint32_t addr, void *buf, uint32_t len) {
    if (!g_ready || !buf) return -1;
    if (addr + len < addr) return -1;
    if (addr + len > (uint32_t)LUGALOS_P4_FLASH_SIZE) return -1;

    uint8_t *out = (uint8_t *)buf;
    ylock_acquire(&g_flash_ylock);
    int rc = 0;
    while (len > 0) {
        uint32_t chunk_addr = addr & ~3u;
        uint32_t skew = addr - chunk_addr;
        uint32_t want = len < (256u - skew) ? len : (256u - skew);
        uint32_t words = (skew + want + 3u) & ~3u;
        uint32_t tmp[64];
        if (words > sizeof(tmp)) words = sizeof(tmp);
        if (rom_read(chunk_addr, tmp, (int32_t)words) != ROM_OK) { rc = -1; break; }
        uint32_t got = words - skew;
        if (got > want) got = want;
        memcpy(out, (const uint8_t *)tmp + skew, got);
        out  += got;
        addr += got;
        len  -= got;
    }
    ylock_release(&g_flash_ylock);
    return rc;
}

int flash_p4_erase_sector(uint32_t addr) {
    if (!g_ready) return -1;
    if (!writable(addr, FLASH_P4_SECTOR_SIZE)) {
        printk("[Flash] Refusing to erase 0x%06x: below the writable floor 0x%06x\n",
               (unsigned)addr, (unsigned)LUGALOS_P4_FLASH_WRITABLE_FLOOR);
        return -1;
    }
    ylock_acquire(&g_flash_ylock);
    int rc = (rom_unlock() == ROM_OK &&
              rom_erase_sector(addr / FLASH_P4_SECTOR_SIZE) == ROM_OK) ? 0 : -1;
    ylock_release(&g_flash_ylock);
    return rc;
}

int flash_p4_write(uint32_t addr, const void *buf, uint32_t len) {
    if (!g_ready || !buf) return -1;
    if (!writable(addr, len)) {
        printk("[Flash] Refusing to write %u bytes at 0x%06x: outside the "
               "writable region\n", (unsigned)len, (unsigned)addr);
        return -1;
    }
    /* Word-aligned address and length, through a bounce buffer, for the same
     * reason the read path has one. Callers hand over 512-byte blocks, so the
     * common case takes the fast path anyway. */
    if ((addr & 3u) != 0) return -1;

    const uint8_t *in = (const uint8_t *)buf;
    ylock_acquire(&g_flash_ylock);
    int rc = 0;
    if (rom_unlock() != ROM_OK) rc = -1;
    while (rc == 0 && len > 0) {
        uint32_t chunk = len < 256u ? len : 256u;
        uint32_t tmp[64];
        memset(tmp, 0xFF, sizeof(tmp));
        memcpy(tmp, in, chunk);
        uint32_t words = (chunk + 3u) & ~3u;
        if (rom_write(addr, tmp, (int32_t)words) != ROM_OK) { rc = -1; break; }
        in   += chunk;
        addr += chunk;
        len  -= chunk;
    }
    ylock_release(&g_flash_ylock);
    return rc;
}
