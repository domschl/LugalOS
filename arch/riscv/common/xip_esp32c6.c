/* Mapping the ESP32-C6's flash into the CPU's address space, at boot.
 * 45.4.2, . The C6 counterpart of
 * arch/riscv/common/xip_esp32p4.c, and everything that file says about why this
 * is deliberately dull applies here.
 *
 * It runs before the kernel exists, from `.boot.text` in SRAM, because until it
 * returns there is nothing at 0x42000000 to fetch: no string literals, no tables,
 * no switch (they go to .rodata, which is in the window not yet mapped), no calls
 * into the kernel, and no console to report on. Only registers and the boot ROM.
 *
 * ## What the probe established (tools/xip_probe_esp32c6.c, 2026-10-05)
 *
 *   * The sequence is IDF's second-stage bootloader's, from set_cache_and_start_app:
 *     cache off, page size, MMU unmapped, entries written, buses un-shut, cache on.
 *   * **A cached read of mapped flash returns 0x0addbad0 (the bus-error marker)
 *     until the ROM's `spi_flash_boot_attach()` has run.** It is what configures the
 *     SPI0 read path (command, dummy cycles, pins) that the cache uses. A board that
 *     boots *from* flash has had it run for it by the ROM; one delivered by
 *     `esptool load-ram` has not, and the flash writes esptool does go through
 *     SPI1 and need none of it. With it, 65 536 words read back exactly.
 *   * The MMU is 256 entries of 64 KB at 0x42000000; an entry is the flash page
 *     number | (1 << 9). I and D share the window.
 *
 * ROM addresses are from esp32c6.rom.ld (IDF components/esp_rom/esp32c6/ld/).
 */

#include <stdint.h>
#include "lugalos_config.h"
#include "drivers/ota_esp32c6.h"

#if defined(CONFIG_BOARD_ESP32C6)

#define BOOT_TEXT __attribute__((section(".boot.text"), noinline))

typedef uint32_t (*rom_u32_void_t)(void);
typedef void     (*rom_void_void_t)(void);
typedef void     (*rom_void_u32_t)(uint32_t);

#define ROM_SPI_FLASH_BOOT_ATTACH ((rom_void_void_t)0x4000026cu)
#define ROM_CACHE_DISABLE_ICACHE  ((rom_u32_void_t)0x40000690u)
#define ROM_CACHE_ENABLE_ICACHE   ((rom_void_u32_t)0x40000694u)
#define ROM_CACHE_INVALIDATE_ALL  ((rom_void_void_t)0x4000064cu)

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define SPI0_BASE           0x60002000u
#define MMU_ITEM_CONTENT    (SPI0_BASE + 0x37cu)
#define MMU_ITEM_INDEX      (SPI0_BASE + 0x380u)
#define MMU_POWER_CTRL      (SPI0_BASE + 0x384u)
#define MMU_PAGE_SIZE_M     (3u << 3)           /* 0 = 64 KB */
#define MMU_VALID           (1u << 9)
#define MMU_ENTRIES         256u

#define EXTMEM_L1_CACHE_CTRL 0x600C8004u
#define CACHE_SHUT_IBUS      (1u << 0)
#define CACHE_SHUT_DBUS      (1u << 1)

/* How many 64 KB pages .text + .rodata occupy -- computed by the linker script,
 * where the section sizes are known (a constant here would map too few pages and
 * fault in whatever landed past the end, with no console to say so). */
extern char _xip_pages[];

/* Which slot this boot came from, for the update backend (drivers/ota_esp32c6.c).
 * In .data, not .bss: it is written before the kernel clears .bss. */
__attribute__((section(".data"))) c6_boot_info_t g_c6_boot = { 0, 0, 0, 0 };

typedef int (*rom_flash_read_t)(uint32_t src, uint32_t *dest, int32_t len);
typedef int (*rom_flash_write_t)(uint32_t dest, const uint32_t *src, int32_t len);
typedef int (*rom_flash_unlock_t)(void);

#define ROM_FLASH_WRITE   ((rom_flash_write_t)0x4000014cu)
#define ROM_FLASH_READ    ((rom_flash_read_t)0x40000150u)
#define ROM_FLASH_UNLOCK  ((rom_flash_unlock_t)0x40000154u)
#define ROM_CACHE_SUSPEND ((rom_u32_void_t)0x40000698u)
#define ROM_CACHE_RESUME  ((rom_void_u32_t)0x4000069cu)

#define WDT_WKEY                 0x50D83AA1u
#define LP_WDT_BASE              0x600B1C00u
#define LP_WDT_CONFIG0           (LP_WDT_BASE + 0x00u)
#define LP_WDT_CONFIG1           (LP_WDT_BASE + 0x04u)
#define LP_WDT_FEED              (LP_WDT_BASE + 0x14u)
#define LP_WDT_WPROTECT          (LP_WDT_BASE + 0x18u)
#define LP_WDT_FLASHBOOT_EN      (1u << 12)
#define LP_WDT_EN                (1u << 31)
#define LP_WDT_STG0_RESET_RTC    (4u << 28)
#define LP_WDT_SYS_RESET_LENGTH  (7u << 13)
#define LP_WDT_CPU_RESET_LENGTH  (7u << 16)
#define RC_SLOW_HZ               136000u
#define LP_WDT_SWD_CONFIG        (LP_WDT_BASE + 0x1cu)
#define LP_WDT_SWD_WPROTECT      (LP_WDT_BASE + 0x20u)
#define LP_WDT_SWD_AUTO_FEED     (1u << 18)

/* `magic` == OTA_C6_BOOT_MAGIC: the loader booted this kernel and names the
 * XIP half to map; otherwise (a direct ROM boot, esptool's load-ram) slot A's. */
BOOT_TEXT void esp32c6_xip_map(uint32_t magic, uint32_t xip_base, uint32_t flags, uint32_t slot) {
    if (magic != OTA_C6_BOOT_MAGIC) {
        uint32_t cache_ctrl = REG(EXTMEM_L1_CACHE_CTRL);
        REG(MMU_ITEM_INDEX) = 0;
        uint32_t mmu0 = REG(MMU_ITEM_CONTENT);
        REG(MMU_ITEM_INDEX) = MMU_ENTRIES - 1u;
        uint32_t mmu_end = REG(MMU_ITEM_CONTENT);
        if ((cache_ctrl & (CACHE_SHUT_IBUS | CACHE_SHUT_DBUS)) == 0 &&
            (mmu0 & MMU_VALID) != 0 && mmu0 == mmu_end) {
            uint32_t part_base = (mmu0 & 0xffu) << 16;
            uint32_t idf_slot = (part_base == 0x010000u) ? 0u : 1u;
            uint32_t idf_flags = 0;
            uint32_t autoload = ROM_CACHE_SUSPEND();
            for (uint32_t i = 0; i < 2u; i++) {
                uint32_t s[8];
                uint32_t sec = 0xd000u + i * 0x1000u;
                if (ROM_FLASH_READ(sec, s, 32) == 0 &&
                    s[0] != 0xffffffffu && s[0] != 0u && s[1] == 0x4c52544cu) {
                    uint32_t entry_slot = (s[0] - 1u) & 1u;
                    if (entry_slot == idf_slot && s[6] == 0xffffffffu) {
                        uint32_t aborted = 4u;
                        if (ROM_FLASH_UNLOCK() == 0) (void)ROM_FLASH_WRITE(sec + 24u, &aborted, 4);
                        idf_flags |= OTA_C6_FLAG_TRIAL;
                    } else if (entry_slot != idf_slot && s[6] == 4u) {
                        uint32_t zero = 0u;
                        if (ROM_FLASH_UNLOCK() == 0) (void)ROM_FLASH_WRITE(sec + 4u, &zero, 4);
                        idf_flags |= OTA_C6_FLAG_FELL_BACK;
                    }
                }
            }
            ROM_CACHE_RESUME(autoload);
            REG(LP_WDT_WPROTECT) = WDT_WKEY;
            if (idf_flags & OTA_C6_FLAG_TRIAL) {
                REG(LP_WDT_CONFIG1) = OTA_C6_TRIAL_S * RC_SLOW_HZ;
                REG(LP_WDT_CONFIG0) = LP_WDT_EN | LP_WDT_STG0_RESET_RTC |
                                      LP_WDT_SYS_RESET_LENGTH | LP_WDT_CPU_RESET_LENGTH;
                REG(LP_WDT_FEED) = 1u << 31;
            } else {
                REG(LP_WDT_CONFIG0) &= ~LP_WDT_EN;
            }
            REG(LP_WDT_WPROTECT) = 0;
            g_c6_boot.magic = OTA_C6_IDF_BOOT_MAGIC;
            g_c6_boot.xip_base = part_base;
            g_c6_boot.flags = idf_flags;
            g_c6_boot.slot = idf_slot;
            return;
        }
    }
    if (magic != OTA_C6_BOOT_MAGIC || xip_base % 0x10000u ||
        (xip_base != (uint32_t)LUGALOS_C6_SLOT_A_BASE + OTA_C6_PART1_OFF &&
         xip_base != (uint32_t)LUGALOS_C6_SLOT_B_BASE + OTA_C6_PART1_OFF)) {
        magic = 0;
        xip_base = (uint32_t)LUGALOS_C6_SLOT_A_BASE + OTA_C6_PART1_OFF;
        flags = 0;
        slot = 0;
    }
    g_c6_boot.magic = magic;
    g_c6_boot.xip_base = xip_base;
    g_c6_boot.flags = flags;
    g_c6_boot.slot = slot;
    ROM_SPI_FLASH_BOOT_ATTACH();

    ROM_CACHE_DISABLE_ICACHE();
    REG(MMU_POWER_CTRL) &= ~MMU_PAGE_SIZE_M;

    for (uint32_t i = 0; i < MMU_ENTRIES; i++) {
        REG(MMU_ITEM_INDEX) = i;
        REG(MMU_ITEM_CONTENT) = 0;
    }

    uint32_t pages = (uint32_t)(uintptr_t)_xip_pages;
    uint32_t first = xip_base >> 16;
    for (uint32_t p = 0; p < pages; p++) {
        REG(MMU_ITEM_INDEX) = p;
        REG(MMU_ITEM_CONTENT) = (first + p) | MMU_VALID;
    }

    REG(EXTMEM_L1_CACHE_CTRL) &= ~(CACHE_SHUT_IBUS | CACHE_SHUT_DBUS);
    ROM_CACHE_INVALIDATE_ALL();
    ROM_CACHE_ENABLE_ICACHE(0);
}

/* --- the flash-boot watchdogs -------------------------------------------
 *
 * A board booted from flash has two watchdogs armed by the ROM for the
 * second-stage image it just loaded; nothing in this kernel feeds them (the P4's
 * recorded the symptom: a kernel that boots completely, reaches its
 * shell, and then resets with a watchdog reason). `load-ram` arms nothing, so
 * this is a no-op on the development loop and essential on the final one. The
 * same three steps as IDF's bootloader_config_wdt() and bootloader_super_wdt_
 * auto_feed(), the RTC watchdog first so there is no window with neither:
 *
 *   RWDT (LP_WDT): clear the flash-boot enable;  MWDT0 (TIMG0): same;
 *   the super watchdog: auto-feed (it cannot be disabled).
 *
 * All three are write-protected by one key. */
#define WDT_WKEY            0x50D83AA1u
#define LP_WDT_BASE         0x600B1C00u
#define LP_WDT_CONFIG0      (LP_WDT_BASE + 0x00u)
#define LP_WDT_WPROTECT     (LP_WDT_BASE + 0x18u)
#define LP_WDT_FLASHBOOT_EN (1u << 12)
#define LP_WDT_SWD_CONFIG   (LP_WDT_BASE + 0x1cu)
#define LP_WDT_SWD_WPROTECT (LP_WDT_BASE + 0x20u)
#define LP_WDT_SWD_AUTO_FEED (1u << 18)

#define TIMG0_BASE          0x60008000u
#define TIMG0_WDTCONFIG0    (TIMG0_BASE + 0x48u)
#define TIMG0_WDTWPROTECT   (TIMG0_BASE + 0x64u)
#define TIMG0_FLASHBOOT_EN  (1u << 14)

BOOT_TEXT void esp32c6_boot_wdt_disable(void) {
    REG(LP_WDT_WPROTECT) = WDT_WKEY;
    REG(LP_WDT_CONFIG0) &= ~LP_WDT_FLASHBOOT_EN;
    REG(LP_WDT_WPROTECT) = 0;

    REG(TIMG0_WDTWPROTECT) = WDT_WKEY;
    REG(TIMG0_WDTCONFIG0) &= ~TIMG0_FLASHBOOT_EN;
    REG(TIMG0_WDTWPROTECT) = 0;

    REG(LP_WDT_SWD_WPROTECT) = WDT_WKEY;
    REG(LP_WDT_SWD_CONFIG) |= LP_WDT_SWD_AUTO_FEED;
    REG(LP_WDT_SWD_WPROTECT) = 0;
}

#endif /* CONFIG_BOARD_ESP32C6 */
