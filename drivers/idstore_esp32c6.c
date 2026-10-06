/*
 * LugalOS driver: the identity store on the ESP32-C6 -- 45.8,
 * plan/phase45_esp32c6.md; the record itself is plan/phase21_identity_and_
 * authentication.md's, unchanged.
 *
 * One 4 KB flash sector at LUGALOS_C6_IDENTITY_BASE (cmake/flash_layout_
 * esp32c6.cmake), offered to kernel/identity.c as the block device it keeps
 * the record on -- the same seam drivers/idstore_rp2350.c fills. The sector
 * is written by the device and never by a flashing tool: it is not in
 * flash.manifest.
 *
 * ## The ROM does the SPI work
 *
 * As on the P4 (drivers/flash_esp32p4.c), every transfer is a call into the
 * boot ROM's SPI flash routines -- what Espressif's bootloader and esptool's
 * stub use -- at the addresses in IDF's components/esp_rom/esp32c6/ld/
 * esp32c6.rom.ld. Two things differ from the P4, both because this kernel
 * *executes from the flash it writes*:
 *
 *   - the instruction cache is suspended around each ROM call (IDF's
 *     spi_flash_disable_interrupts_caches_and_other_cpu does the same on this
 *     chip): the ROM drives the flash through SPI1 while the cache fetches
 *     through SPI0, and the two must not meet;
 *   - so everything that runs while it is suspended -- the guard functions
 *     below -- is in .ramfunc, with interrupts masked, because every handler
 *     lives in flash. The buffers they touch are on the caller's stack, in
 *     SRAM.
 *
 * Reads are guarded as strictly as writes: a ROM read holds the bus for only
 * microseconds, but an instruction fetch landing in those microseconds gets
 * nothing (the P4 learned that the expensive way; its comment tells it).
 *
 * ## The ROM's idea of the chip
 *
 * The ROM bounds-checks every address against its own record of the chip's
 * size, which it took from the boot image's header: 2 MB here, where the part
 * is 8 MB (read off the board: rom_spiflash_legacy_data->chip.chip_size =
 * 0x200000). An identity sector at 0x7F0000 is refused until that record says
 * 8 MB, so the first use sets it with esp_rom_spiflash_config_param -- which
 * changes nothing but that record. XIP does not consult it: the MMU maps
 * flash pages, it does not ask the ROM.
 */

#include "drivers/block.h"
#include "kernel/identity.h"
#include "kernel/idstore.h"
#include "kernel/lock.h"
#include "kernel/printk.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if !defined(LUGALOS_C6_IDENTITY_BASE) || !defined(LUGALOS_C6_FLASH_SIZE)
#error "the C6 flash map (cmake/flash_layout_esp32c6.cmake) defines where the identity sector lives"
#endif

/* --- the ROM entry points (esp32c6.rom.ld) ------------------------------ */

typedef int      (*rom_config_param_fn)(uint32_t device_id, uint32_t chip_size, uint32_t block_size,
                                        uint32_t sector_size, uint32_t page_size, uint32_t status_mask);
typedef int      (*rom_erase_sector_fn)(uint32_t sector_num);
typedef int      (*rom_write_fn)(uint32_t dest_addr, const uint32_t *src, int32_t len);
typedef int      (*rom_read_fn)(uint32_t src_addr, uint32_t *dest, int32_t len);
typedef int      (*rom_unlock_fn)(void);
typedef uint32_t (*rom_cache_suspend_fn)(void);
typedef void     (*rom_cache_resume_fn)(uint32_t autoload);

#define rom_config_param  ((rom_config_param_fn)0x40000160UL)   /* esp_rom_spiflash_config_param */
#define rom_erase_sector  ((rom_erase_sector_fn)0x40000144UL)   /* esp_rom_spiflash_erase_sector */
#define rom_write         ((rom_write_fn)0x4000014cUL)          /* esp_rom_spiflash_write */
#define rom_read          ((rom_read_fn)0x40000150UL)           /* esp_rom_spiflash_read */
#define rom_unlock        ((rom_unlock_fn)0x40000154UL)         /* esp_rom_spiflash_unlock */
#define rom_cache_suspend ((rom_cache_suspend_fn)0x40000698UL)  /* Cache_Suspend_ICache */
#define rom_cache_resume  ((rom_cache_resume_fn)0x4000069cUL)   /* Cache_Resume_ICache */

/* rom_spiflash_legacy_data: a pointer, in the ROM's reserved top of SRAM, to the
 * structure whose first member is the chip record (esp_rom_spiflash_chip_t:
 * device id, chip size, block, sector and page size, status mask). */
#define ROM_SPIFLASH_LEGACY_DATA (*(uint32_t *volatile *)0x4087ffecUL)

#define ROM_OK       0
#define SECTOR_BYTES 4096u
#define CHUNK_BYTES  256u          /* one flash page; also the stack buffer per transfer */

/* --- the guard: interrupts off, cache suspended, all in SRAM --------------
 *
 * The function pointers are passed in rather than named inside: a call
 * through a constant address compiles to a load of that constant, and GCC is
 * free to place the constant in .rodata -- in flash, unreachable while the
 * cache is suspended. Arguments arrive in registers. */

typedef struct {
    rom_cache_suspend_fn suspend;
    rom_cache_resume_fn  resume;
    rom_unlock_fn        unlock;
    rom_erase_sector_fn  erase_sector;
    rom_write_fn         write;
    rom_read_fn          read;
    rom_config_param_fn  config_param;
} rom_fns_t;

enum { OP_READ, OP_WRITE, OP_ERASE, OP_CONFIG };

__attribute__((section(".ramfunc"), noinline))
static int guarded(const rom_fns_t *f, int op, uint32_t addr, uint32_t *words, uint32_t nbytes, uint32_t dev_id) {
    uint32_t mstatus;
    __asm__ __volatile__("csrrci %0, mstatus, 0x8" : "=r"(mstatus));
    uint32_t autoload = f->suspend();
    int rc;
    /* An if-chain, not a switch: a switch may become a jump table in .rodata -- in flash. */
    if (op == OP_READ) {
        rc = f->read(addr, words, (int32_t)nbytes);
    } else if (op == OP_WRITE) {
        rc = f->unlock();
        if (rc == ROM_OK) rc = f->write(addr, words, (int32_t)nbytes);
    } else if (op == OP_ERASE) {
        rc = f->unlock();
        if (rc == ROM_OK) rc = f->erase_sector(addr / SECTOR_BYTES);
    } else {
        rc = f->config_param(dev_id, nbytes, 65536u, SECTOR_BYTES, CHUNK_BYTES, 0xffffu);
    }
    f->resume(autoload);
    if (mstatus & 0x8u) __asm__ __volatile__("csrsi mstatus, 0x8");
    return rc == ROM_OK ? 0 : -1;
}

static void fns_init(rom_fns_t *f) {
    f->suspend = rom_cache_suspend;
    f->resume = rom_cache_resume;
    f->unlock = rom_unlock;
    f->erase_sector = rom_erase_sector;
    f->write = rom_write;
    f->read = rom_read;
    f->config_param = rom_config_param;
}

static ylock_t g_lock;
static bool g_ready;

/* Once: tell the ROM the part is the size the flash map says it is. */
static bool flash_ready(const rom_fns_t *f) {
    if (g_ready) return true;
    const uint32_t *chip = ROM_SPIFLASH_LEGACY_DATA;
    uint32_t dev_id = chip ? chip[0] : 0;
    if (guarded(f, OP_CONFIG, 0, NULL, (uint32_t)LUGALOS_C6_FLASH_SIZE, dev_id) != 0) {
        printk("[IdStore] the ROM refused the flash parameters\n");
        return false;
    }
    g_ready = true;
    return true;
}

/* --- the store, as a block device ------------------------------------- */

static int idflash_read(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if ((uint64_t)lba + count > IDSTORE_BLOCKS) return -1;
    rom_fns_t f;
    fns_init(&f);
    uint32_t addr = (uint32_t)LUGALOS_C6_IDENTITY_BASE + lba * IDSTORE_BLOCK_SIZE;
    uint32_t left = count * IDSTORE_BLOCK_SIZE;
    uint8_t *out = buf;
    int rc = 0;
    ylock_acquire(&g_lock);
    if (!flash_ready(&f)) rc = -1;
    while (rc == 0 && left) {
        uint32_t tmp[CHUNK_BYTES / 4];
        uint32_t n = left < CHUNK_BYTES ? left : CHUNK_BYTES;
        if (guarded(&f, OP_READ, addr, tmp, n, 0) != 0) { rc = -1; break; }
        memcpy(out, tmp, n);
        out += n; addr += n; left -= n;
    }
    ylock_release(&g_lock);
    return rc;
}

/* Whole-sector writes only, as on the RP2350 and for its reason: flash erases
 * 4 KB at a time, a partial write would need a 4 KB read-modify-write buffer,
 * and the only writer (idstore_writer_commit) always writes the whole record. */
static int idflash_write(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (lba != 0 || count != IDSTORE_BLOCKS) {
        printk("[IdStore] partial write refused (lba %u, %u blocks): only the whole %u-block sector\n",
               (unsigned)lba, (unsigned)count, (unsigned)IDSTORE_BLOCKS);
        return -1;
    }
    /* The guard on the one place that touches the hardware: nothing below the
     * identity sector -- the second stage, the OS image, /flash0 -- can be erased
     * from here, whatever a caller computes. */
    const uint32_t base = (uint32_t)LUGALOS_C6_IDENTITY_BASE;
    if (base % SECTOR_BYTES || base + SECTOR_BYTES > (uint32_t)LUGALOS_C6_FLASH_SIZE ||
        base < (uint32_t)LUGALOS_C6_OSIMAGE_BASE + (uint32_t)LUGALOS_C6_OSIMAGE_SIZE) {
        printk("[IdStore] the identity sector is not where it may be written\n");
        return -1;
    }

    rom_fns_t f;
    fns_init(&f);
    printk("[IdStore] writing the identity sector\n");
    int rc = 0;
    ylock_acquire(&g_lock);
    if (!flash_ready(&f) || guarded(&f, OP_ERASE, base, NULL, 0, 0) != 0) rc = -1;
    const uint8_t *in = buf;
    for (uint32_t off = 0; rc == 0 && off < SECTOR_BYTES; off += CHUNK_BYTES) {
        uint32_t tmp[CHUNK_BYTES / 4];
        memcpy(tmp, in + off, CHUNK_BYTES);
        if (guarded(&f, OP_WRITE, base + off, tmp, CHUNK_BYTES, 0) != 0) rc = -1;
    }
    /* Read back what landed: a store that reports success without having stored
     * anything is the failure that matters most for a key (phase 21 §7). */
    for (uint32_t off = 0; rc == 0 && off < SECTOR_BYTES; off += CHUNK_BYTES) {
        uint32_t tmp[CHUNK_BYTES / 4];
        if (guarded(&f, OP_READ, base + off, tmp, CHUNK_BYTES, 0) != 0 || memcmp(tmp, in + off, CHUNK_BYTES) != 0) {
            printk("[IdStore] write did not verify at +0x%x\n", (unsigned)off);
            rc = -1;
        }
    }
    ylock_release(&g_lock);
    return rc;
}

static block_dev_t g_idstore_dev = {
    .name = "idstore0",
    .block_size = IDSTORE_BLOCK_SIZE,
    .num_blocks = IDSTORE_BLOCKS,
    .read_blocks = idflash_read,
    .write_blocks = idflash_write,
};

/* Strong definition of kernel/identity.c's weak hook. */
block_dev_t *identity_store_device(void) {
    static bool inited;
    if (!inited) { ylock_init(&g_lock); inited = true; }
    return &g_idstore_dev;
}
