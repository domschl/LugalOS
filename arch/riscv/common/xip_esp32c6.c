/* Mapping the ESP32-C6's flash into the CPU's address space, at boot.
 * 45.4.2, plan/phase45_esp32c6.md. The C6 counterpart of
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

BOOT_TEXT void esp32c6_xip_map(void) {
    ROM_SPI_FLASH_BOOT_ATTACH();

    ROM_CACHE_DISABLE_ICACHE();
    REG(MMU_POWER_CTRL) &= ~MMU_PAGE_SIZE_M;

    for (uint32_t i = 0; i < MMU_ENTRIES; i++) {
        REG(MMU_ITEM_INDEX) = i;
        REG(MMU_ITEM_CONTENT) = 0;
    }

    uint32_t pages = (uint32_t)(uintptr_t)_xip_pages;
    uint32_t first = (uint32_t)LUGALOS_C6_OSIMAGE_BASE >> 16;
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
 * phase 32 recorded the symptom: a kernel that boots completely, reaches its
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
