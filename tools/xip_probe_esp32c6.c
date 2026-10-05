/*
 * Flash-mapping probe for the ESP32-C6 -- 45.4.2, plan/phase45_esp32c6.md.
 *
 * Can a program loaded to SRAM by `esptool load-ram` map flash into the CPU's
 * address space and execute/read it? The kernel is 540 KB and the SRAM is 496,
 * so the answer decides the whole boot design. Sequence, from IDF's C6 second-
 * stage bootloader (components/bootloader_support/src/bootloader_utility.c
 * set_cache_and_start_app, hal/esp32c6/include/hal/{mmu,cache}_ll.h):
 *
 *   disable the cache; set the MMU page size (64 KB); invalidate every MMU
 *   entry; write the entries that map virtual 0x42000000.. onto flash pages;
 *   un-shut the instruction and data buses; enable the cache.
 *
 * Then read the window and compare with the pattern written to flash at 0x20000
 * (word k of the image = 0xC6000000 ^ k ^ 0x5a5a0000). The program prints what it
 * finds at each step so a failure says where. Standalone, loaded to RAM;
 * flash is read, never written.
 */
#include "c6_standalone.h"

#define SPI0_BASE          0x60002000u
#define MMU_ITEM_CONTENT   (SPI0_BASE + 0x37c)
#define MMU_ITEM_INDEX     (SPI0_BASE + 0x380)
#define MMU_POWER_CTRL     (SPI0_BASE + 0x384)
#define MMU_PAGE_SIZE_S    3
#define MMU_PAGE_SIZE_M    (3u << MMU_PAGE_SIZE_S)
#define MMU_VALID          (1u << 9)
#define MMU_ENTRIES        256u

#define EXTMEM_L1_CACHE_CTRL 0x600C8004u
#define SHUT_IBUS (1u << 0)
#define SHUT_DBUS (1u << 1)

/* ROM, from esp32c6.rom.ld */
typedef uint32_t (*rom_disable_t)(void);
typedef void (*rom_enable_t)(uint32_t);
typedef void (*rom_void_t)(void);
#define ROM_CACHE_DISABLE_ICACHE ((rom_disable_t)0x40000690u)
#define ROM_CACHE_ENABLE_ICACHE  ((rom_enable_t)0x40000694u)
#define ROM_CACHE_INVALIDATE_ALL ((rom_void_t)0x4000064cu)

#define FLASH_PADDR 0x20000u
#define VBASE       0x42000000u

static uint32_t expect(uint32_t byte_off) { return 0xC6000000u ^ (byte_off >> 2) ^ 0x5a5a0000u; }

static void mmu_write(uint32_t id, uint32_t val) { REG(MMU_ITEM_INDEX) = id; REG(MMU_ITEM_CONTENT) = val; }
static uint32_t mmu_read(uint32_t id) { REG(MMU_ITEM_INDEX) = id; return REG(MMU_ITEM_CONTENT); }

static void dump_state(const char *tag) {
    puts_(tag);
    puts_(": page_size_field="); puthex((REG(MMU_POWER_CTRL) >> MMU_PAGE_SIZE_S) & 3u);
    puts_(" L1_CACHE_CTRL="); puthex(REG(EXTMEM_L1_CACHE_CTRL));
    puts_(" mmu[0]="); puthex(mmu_read(0));
    puts_(" mmu[1]="); puthex(mmu_read(1));
    puts_(" mmu[255]="); puthex(mmu_read(255));
    puts_("\n");
}

void minimal_main(void) {
    REG(SYSTIMER_CONF_REG) |= SYSTIMER_CLK_EN | SYSTIMER_UNIT0_WORK_EN;
    for (;;) {
        puts_("\n[C6_XIP] flash-mapping probe\n");
        dump_state("  as the ROM left it");

        /* 0. what the ROM's own boot-time cache init does: it maps something and
         * reads flash through it, so if THIS reads sensible bytes the flash path
         * works and our own sequence is what is missing. */
        /* The ROM's own "attach the flash for booting": configures the SPI0
         * read path (pins, command, dummy cycles) that a cached read uses. In
         * download mode nothing has run it -- the flash writes esptool does go
         * through SPI1 commands and do not need it. */
        ((rom_void_t)0x4000026cu)();                         /* spi_flash_boot_attach */
        ((rom_void_t)0x40000634u)();                         /* ROM_Boot_Cache_Init */
        dump_state("  ROM_Boot_Cache_Init");
        {
            volatile uint32_t *r = (volatile uint32_t *)VBASE;
            puts_("  window[0..3] after the ROM's init: "); puthex(r[0]); puts_(" "); puthex(r[1]); puts_(" "); puthex(r[2]); puts_(" "); puthex(r[3]); puts_("\n");
        }

        /* 1. cache off, MMU page size, clear the table */
        ROM_CACHE_DISABLE_ICACHE();
        REG(MMU_POWER_CTRL) = (REG(MMU_POWER_CTRL) & ~MMU_PAGE_SIZE_M);          /* 0 = 64 KB */
        for (uint32_t i = 0; i < MMU_ENTRIES; i++) mmu_write(i, 0);
        dump_state("  after unmap     ");

        /* 2. map 4 pages (256 KB): vaddr 0x42000000.. -> flash 0x20000.. */
        for (uint32_t p = 0; p < 4; p++) mmu_write(p, ((FLASH_PADDR >> 16) + p) | MMU_VALID);
        dump_state("  after mapping   ");

        /* 3. buses on, cache on */
        REG(EXTMEM_L1_CACHE_CTRL) &= ~(SHUT_IBUS | SHUT_DBUS);
        ROM_CACHE_INVALIDATE_ALL();
        ROM_CACHE_ENABLE_ICACHE(0);
        dump_state("  cache enabled   ");

        /* 4. read through the window and compare, as data */
        volatile uint32_t *w = (volatile uint32_t *)VBASE;
        uint32_t bad = 0, first_bad = 0xffffffffu, checked = 0;
        for (uint32_t k = 0; k < 0x40000; k += 4) {
            uint32_t got = w[k >> 2];
            checked++;
            if (got != expect(k)) { if (!bad) first_bad = k; bad++; }
        }
        puts_("  data read: "); putdec(checked); puts_(" words, "); putdec(bad); puts_(" wrong");
        if (bad) { puts_(", first at +"); puthex(first_bad); puts_(" got "); puthex(w[first_bad >> 2]); puts_(" want "); puthex(expect(first_bad)); }
        puts_(bad ? "  FAIL\n" : "  PASS\n");

        /* 5. and execute from it: a tiny function copied... no -- jump to code that
         * was never in SRAM. The pattern is not code, so instead check the second
         * property executing needs: that instruction fetch and data read agree on the
         * same bytes, by reading the first word back after the cache has been
         * invalidated (a stale line would show). */
        ROM_CACHE_INVALIDATE_ALL();
        puts_("  re-read after invalidate: "); puthex(w[0]); puts_(w[0] == expect(0) ? "  PASS\n" : "  FAIL\n");

        uint32_t t = systimer_now();
        while ((uint32_t)(systimer_now() - t) < 16000000u * 3u) { }
    }
}
