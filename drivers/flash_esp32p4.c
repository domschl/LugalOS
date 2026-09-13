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

/* --- writing flash while executing from it (U3) --------------------------
 *
 * plan/phase32_esp32p4_execute_in_place.md §2. Before phase 32 this file had
 * no such problem: the kernel was loaded into L2MEM and flash was only ever a
 * transfer. Now `.text` and `.rodata` are fetched from the XIP window, and an
 * erase or program makes the flash chip stop answering reads for as long as
 * it takes -- tens of milliseconds for a sector erase.
 *
 * So during that window **no instruction may be fetched from flash**, and the
 * failure if one is does not look like an error: the fetch returns whatever
 * the busy chip drives, and the CPU executes it. A corrupted filesystem or a
 * jump into nothing, not a return code.
 *
 * ESP-IDF solves this with two steps, in
 * components/spi_flash/cache_utils.c's
 * spi_flash_disable_interrupts_caches_and_other_cpu():
 *
 *   1. `esp_intr_noniram_disable()` -- disable the interrupts whose handlers
 *      are *not* in IRAM, leaving IRAM-resident ones running.
 *   2. `spi_flash_disable_cache()`.
 *
 * Both specialise here, and in opposite directions:
 *
 * **Step 1 becomes a blanket mask, because this kernel has no RAM-resident
 * handler to spare.** IDF can be selective; we cannot. `trap_vector_entry` is
 * in RAM (it rides in .boot with entry.S), but the `trap_handler` it calls is
 * at 0x40010468 -- in flash -- so an interrupt arriving mid-erase gets three
 * instructions into the vector and then jumps into a chip that is busy
 * erasing. Masking mstatus.MIE is therefore the honest equivalent of IDF's
 * selective disable, not a lazier version of it. It costs a few ticks of
 * latency per erase.
 *
 * **Step 2 is not needed at all, and the reason is the writable floor.** A
 * cache disable exists to stop stale lines answering for flash that has just
 * changed underneath them. This kernel can only ever write at or above
 * LUGALOS_P4_FLASH_WRITABLE_FLOOR (0x00110000), and the XIP window maps
 * LUGALOS_P4_OSIMAGE_BASE..+SIZE (0x00010000..0x00110000) -- the floor is
 * exactly the top of the mapped region. **No byte this driver can write is
 * ever mapped**, so nothing cached can go stale, and the `writable()` check a
 * few lines up is load-bearing for correctness rather than merely for safety.
 * (The OS image is only ever rewritten by tools/p4flash.py, from download
 * mode, with no kernel running to have cached anything.)
 *
 * Two things are enforced rather than remembered:
 *
 *   * The routines are in `.ramfunc`, which linker/esp32p4.ld places in
 *     L2MEM and ASSERTs is not in the flash window. `noinline` matters as
 *     much as the section: without it the compiler may inline the body back
 *     into its flash-resident caller and silently undo the whole thing --
 *     the same note drivers/flash_rp2350.c carries for the identical hazard.
 *   * The ROM pointers are resolved before entry and passed in as data, so
 *     nothing inside the window looks anything up. The ROM itself is at
 *     0x4fc00000 and is unaffected by the flash being busy.
 *
 * The closure is checkable rather than asserted. `objdump -d --section=.ramfunc`
 * shows every target inside it: direct jumps to 0x4ff4xxxx (L2MEM), and the
 * ROM entered either by an immediate (0x4fc0015c) or by a pointer loaded from
 * the caller's stack. Nothing at 0x4000_0000+. If that ever stops being true,
 * the section moved or something got inlined.
 */

typedef struct {
    rom_unlock_fn       unlock;
    rom_erase_sector_fn erase_sector;
    rom_write_fn        write;
} flash_rom_fns_t;

/* Masks machine interrupts and returns the previous MIE bit, exactly as
 * drivers/flash_rp2350.c does: csrrci returns the prior mstatus, so a caller
 * that already had them off is left that way. */
__attribute__((section(".ramfunc"), noinline))
static uint32_t ram_irq_mask(void) {
    uint32_t saved;
    __asm__ __volatile__("csrrci %0, mstatus, 0x8" : "=r"(saved));
    return saved & 0x8u;
}

__attribute__((section(".ramfunc"), noinline))
static void ram_irq_restore(uint32_t prev) {
    if (prev) __asm__ __volatile__("csrsi mstatus, 0x8");
}

__attribute__((section(".ramfunc"), noinline))
static int flash_erase_ram(const flash_rom_fns_t *f, uint32_t sector) {
    uint32_t prev = ram_irq_mask();
    int rc = (f->unlock() == ROM_OK &&
              f->erase_sector(sector) == ROM_OK) ? 0 : -1;
    ram_irq_restore(prev);
    return rc;
}

/* One chunk per call, with the mask taken and dropped around each, so a
 * multi-chunk write does not hold interrupts off for the whole transfer --
 * between chunks the flash is idle again and the tick can be serviced. */
__attribute__((section(".ramfunc"), noinline))
static int flash_program_ram(const flash_rom_fns_t *f, uint32_t addr,
                             const uint32_t *words, uint32_t nbytes) {
    uint32_t prev = ram_irq_mask();
    int rc = (f->unlock() == ROM_OK &&
              f->write(addr, words, (int32_t)nbytes) == ROM_OK) ? 0 : -1;
    ram_irq_restore(prev);
    return rc;
}

static void flash_rom_fns_init(flash_rom_fns_t *f) {
    f->unlock       = rom_unlock;
    f->erase_sector = rom_erase_sector;
    f->write        = rom_write;
}

int flash_p4_erase_sector(uint32_t addr) {
    if (!g_ready) return -1;
    if (!writable(addr, FLASH_P4_SECTOR_SIZE)) {
        printk("[Flash] Refusing to erase 0x%06x: below the writable floor 0x%06x\n",
               (unsigned)addr, (unsigned)LUGALOS_P4_FLASH_WRITABLE_FLOOR);
        return -1;
    }
    flash_rom_fns_t f;
    flash_rom_fns_init(&f);
    ylock_acquire(&g_flash_ylock);
    int rc = flash_erase_ram(&f, addr / FLASH_P4_SECTOR_SIZE);
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
    flash_rom_fns_t f;
    flash_rom_fns_init(&f);
    ylock_acquire(&g_flash_ylock);
    int rc = 0;
    while (rc == 0 && len > 0) {
        uint32_t chunk = len < 256u ? len : 256u;
        uint32_t tmp[64];
        /* The bounce buffer is filled *outside* the masked window, on
         * purpose: memset and memcpy are flash-resident, and there is no
         * reason for them to run while the chip is busy. By the time
         * flash_program_ram() is entered every byte it needs is on the
         * stack, which is L2MEM. */
        memset(tmp, 0xFF, sizeof(tmp));
        memcpy(tmp, in, chunk);
        uint32_t words = (chunk + 3u) & ~3u;
        if (flash_program_ram(&f, addr, tmp, words) != 0) { rc = -1; break; }
        in   += chunk;
        addr += chunk;
        len  -= chunk;
    }
    ylock_release(&g_flash_ylock);
    return rc;
}
