/* The ESP32-C6's flash, written while the kernel executes from it.
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
 *   - so everything that runs while it is suspended -- guarded() below -- is
 *     in .ramfunc, with interrupts masked, because every handler lives in
 *     flash. The buffers it touches are on the caller's stack, in SRAM.
 *
 * Reads are guarded as strictly as writes: a ROM read holds the bus for only
 * microseconds, but an instruction fetch landing in those microseconds gets
 * nothing (the P4 learned that the expensive way; its comment tells it).
 *
 * A sector erase holds the bus for tens of milliseconds (45 ms typical, the
 * datasheets allow several hundred) with interrupts masked: a caller erasing
 * many sectors yields between them.
 *
 * ## The ROM's idea of the chip
 *
 * The ROM bounds-checks every address against its own record of the chip's
 * size, which it took from the boot image's header -- a kernel loaded by
 * esptool may find 2 MB there on an 8 MB part. So the first use sets it from
 * the flash map with esp_rom_spiflash_config_param, which changes nothing but
 * that record. XIP does not consult it: the MMU maps flash pages, it does not
 * ask the ROM. */

#include "drivers/flash_esp32c6.h"
#include "drivers/ota_esp32c6.h"
#include "kernel/lock.h"
#include "kernel/printk.h"
#include <string.h>

#if !defined(LUGALOS_C6_IDENTITY_BASE) || !defined(LUGALOS_C6_FLASH_SIZE) || !defined(LUGALOS_C6_SLOT_A_BASE)
#error "the C6 flash map (cmake/flash_layout_esp32c6*.cmake) defines where everything lives"
#endif

extern c6_boot_info_t g_c6_boot;                /* arch/riscv/common/xip_esp32c6.c */

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

/* Once, under g_lock: tell the ROM the part is the size the flash map says. */
static bool flash_ready(const rom_fns_t *f) {
    if (g_ready) return true;
    const uint32_t *chip = ROM_SPIFLASH_LEGACY_DATA;
    uint32_t dev_id = chip ? chip[0] : 0;
    if (guarded(f, OP_CONFIG, 0, NULL, (uint32_t)LUGALOS_C6_FLASH_SIZE, dev_id) != 0) {
        printk("[Flash] the ROM refused the flash parameters\n");
        return false;
    }
    g_ready = true;
    return true;
}

static bool inside(uint32_t addr, uint32_t len, uint32_t base, uint32_t size) {
    return addr >= base && len <= size && addr - base <= size - len;
}

bool c6flash_writable(uint32_t addr, uint32_t len) {
    if (len == 0 || addr >= (uint32_t)LUGALOS_C6_FLASH_SIZE) return false;
    if (inside(addr, len, LUGALOS_C6_IDENTITY_BASE, SECTOR_BYTES)) return true;
    if (g_c6_boot.magic == OTA_C6_IDF_BOOT_MAGIC) return ota_c6_idf_writable(addr, len);
    if (inside(addr, len, LUGALOS_C6_BOOTREC_BASE, 2u * SECTOR_BYTES)) return true;
    if (g_c6_boot.magic != OTA_C6_BOOT_MAGIC) return false;
    uint32_t other = g_c6_boot.slot == 0 ? LUGALOS_C6_SLOT_B_BASE : LUGALOS_C6_SLOT_A_BASE;
    return inside(addr, len, other, LUGALOS_C6_SLOT_SIZE);
}

static ylock_t *lock(void) {
    static bool inited;
    if (!inited) { ylock_init(&g_lock); inited = true; }
    return &g_lock;
}

int c6flash_read(uint32_t addr, void *buf, uint32_t len) {
    if (addr >= (uint32_t)LUGALOS_C6_FLASH_SIZE || len > (uint32_t)LUGALOS_C6_FLASH_SIZE - addr) return -1;
    rom_fns_t f;
    fns_init(&f);
    uint8_t *out = buf;
    int rc = 0;
    ylock_acquire(lock());
    if (!flash_ready(&f)) rc = -1;
    while (rc == 0 && len) {
        uint32_t tmp[CHUNK_BYTES / 4];
        uint32_t lead = addr & 3u;
        uint32_t n = len + lead < CHUNK_BYTES ? len : CHUNK_BYTES - lead;
        if (guarded(&f, OP_READ, addr - lead, tmp, (n + lead + 3u) & ~3u, 0) != 0) { rc = -1; break; }
        memcpy(out, (uint8_t *)tmp + lead, n);
        out += n; addr += n; len -= n;
    }
    ylock_release(lock());
    return rc;
}

int c6flash_erase_sector(uint32_t addr) {
    if (addr % SECTOR_BYTES || !c6flash_writable(addr, SECTOR_BYTES)) {
        printk("[Flash] erase refused at 0x%06x\n", (unsigned)addr);
        return -1;
    }
    rom_fns_t f;
    fns_init(&f);
    ylock_acquire(lock());
    int rc = (!flash_ready(&f) || guarded(&f, OP_ERASE, addr, NULL, 0, 0) != 0) ? -1 : 0;
    ylock_release(lock());
    return rc;
}

/* Programs `len` bytes at `addr` (word-aligned, a multiple of 4) into erased
 * flash. Never crosses a 256-byte page in one ROM call. */
int c6flash_write(uint32_t addr, const void *buf, uint32_t len) {
    if ((addr | len) & 3u || !c6flash_writable(addr, len)) {
        printk("[Flash] write refused at 0x%06x (+%u)\n", (unsigned)addr, (unsigned)len);
        return -1;
    }
    rom_fns_t f;
    fns_init(&f);
    const uint8_t *in = buf;
    int rc = 0;
    ylock_acquire(lock());
    if (!flash_ready(&f)) rc = -1;
    while (rc == 0 && len) {
        uint32_t tmp[CHUNK_BYTES / 4];
        uint32_t n = CHUNK_BYTES - (addr % CHUNK_BYTES);
        if (n > len) n = len;
        memcpy(tmp, in, n);
        if (guarded(&f, OP_WRITE, addr, tmp, n, 0) != 0) rc = -1;
        in += n; addr += n; len -= n;
    }
    ylock_release(lock());
    return rc;
}
