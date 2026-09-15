/* Mapping the ESP32-P4's flash into the CPU's address space, at boot.
 * U2, plan/phase32_esp32p4_execute_in_place.md.
 *
 * This file runs before the kernel exists. Everything in it is in `.boot.text`
 * and therefore in L2MEM, because until the function below returns there is
 * nothing at 0x40000000 to fetch -- the code that maps flash cannot itself
 * live in flash.
 *
 * That constraint is stronger than it looks, and it is why this file is
 * deliberately dull:
 *
 *   * **No string literals, no arrays, no switch.** Those go to .rodata,
 *     which is in the flash window and not mapped yet. A `printk` here would
 *     fetch its format string from unmapped space. There is nothing to print
 *     to at this point anyway -- the console is several hundred milliseconds
 *     away -- so this function reports nothing and cannot fail visibly.
 *   * **No calls into the kernel.** Every callee would be in flash. The only
 *     calls are into the boot ROM, which is at 0x4fc00000 and always
 *     present.
 *
 * The ROM addresses are transcribed from ESP-IDF's own ROM linker script,
 * components/esp_rom/esp32p4/ld/esp32p4.rom.ld, the same source and the same
 * method drivers/flash_esp32p4.c and arch/riscv/common/trap.c already use.
 */

#include <stdint.h>
#include "lugalos_config.h"

#if defined(CONFIG_BOARD_ESP32P4)

#define BOOT_TEXT __attribute__((section(".boot.text"), noinline))

/* ROM entry points. Signatures from esp32p4/rom/cache.h. */
#define ROM_CACHE_DISABLE_L1_ICACHE0  0x4fc004d0u
#define ROM_CACHE_ENABLE_L1_ICACHE0   0x4fc004d4u
#define ROM_CACHE_DISABLE_L1_DCACHE   0x4fc004f0u
#define ROM_CACHE_ENABLE_L1_DCACHE    0x4fc004f4u
#define ROM_CACHE_DISABLE_L2_CACHE    0x4fc00500u
#define ROM_CACHE_ENABLE_L2_CACHE     0x4fc00504u
#define ROM_CACHE_INVALIDATE_ALL      0x4fc00404u
/* Cache_WriteBack_All. Address taken from esp-idf's own ROM linker script
 * (components/esp_rom/esp32p4/ld/esp32p4.rom.ld:198), which both the base and
 * the eco0_4 variant agree on -- and the invalidate address above matches
 * that file's Cache_Invalidate_All exactly, which is what makes the pair
 * trustworthy rather than inferred. */
#define ROM_CACHE_WRITEBACK_ALL       0x4fc00414u
#define ROM_CACHE_FLASH_MMU_SET       0x4fc00518u

/* CACHE_MAP_* from esp32p4/rom/cache.h: ICACHE_0 bit 0, ICACHE_1 bit 1,
 * DCACHE bit 4, L2 bit 5. */
#define CACHE_MAP_L1_ICACHE_0   (1u << 0)
#define CACHE_MAP_L1_DCACHE     (1u << 4)
#define CACHE_MAP_L2_CACHE      (1u << 5)

/* The MMU's page size, in kilobytes. cache.h is explicit: "page size of
 * ICache, in kilobytes. Should be 64 here." */
#define MMU_PAGE_KB     64u

/* Supplied by the linker: how many 64 KB pages .text + .rodata occupy.
 *
 * Computed there rather than here because that is where the section sizes are
 * known. A constant in this file would be a number to keep in step by hand,
 * and getting it too small would map fewer pages than the image needs and
 * fault on whatever landed past the end -- with no console to say so. */
extern char _xip_pages[];

/* --- how to debug this function, when it needs it ---------------------
 *
 * Nothing here can report anything. printk needs a format string, which is
 * .rodata, which is in the window this file exists to map; and the console
 * driver is several hundred milliseconds and a great deal of flash-resident
 * code away. A failure is therefore a board that resets in a loop with no
 * output, which is exactly what happened twice while this was written.
 *
 * What worked both times, and what to do again: write single characters
 * straight into UART0's transmit FIFO at 0x500CA000, using immediate
 * addresses and immediate characters so that no .rodata and no kernel code
 * is touched. The ROM has already configured UART0 at 115200 for its own
 * banner, so the pad and baud rate are live before we arrive. A handful of
 * markers between the steps below, plus an 8-digit hex dump of the MMU
 * call's return value, located both bugs in one run each.
 *
 * The markers are not left in. They cost nothing to re-add and their absence
 * is one less thing between a reader and what this function actually does.
 */

/* --- the flash-boot watchdogs ------------------------------------------
 *
 * The ROM arms two watchdogs before jumping to a second-stage image, and they
 * exist to reset a board whose bootloader never finishes. Nothing in this
 * kernel fed them before phase 32, because nothing in this kernel was ever a
 * second-stage image -- `load-ram` hands control over by a different path
 * that arms nothing.
 *
 * Measured, and worth recording because the symptom points away from the
 * cause: with these left armed the kernel booted **completely** -- shell
 * online, /flash0 mounted, stdlib.lisp loaded, timestamps normal -- and then
 * the board reset, over and over, with `rst:0x7 (HP_SYS_HP_WDT_RESET)`. A
 * boot that works and then resets looks like a crash late in init, and it is
 * not: it is a timer that was running before our first instruction.
 *
 * ESP-IDF disables the same two in bootloader_config_wdt(), by the same
 * means and in the same order -- RWDT first, then MWDT0, so there is no
 * window with neither.
 *
 * Both are write-protected by a magic key, and both flashboot bits default
 * to 1 (the register headers say so), which is the ROM arming them rather
 * than anything we did.
 */
#define LP_WDT_BASE         0x50116000u     /* LPAON + 0x6000 */
#define LP_WDT_CONFIG0      (LP_WDT_BASE + 0x00u)
#define LP_WDT_WPROTECT     (LP_WDT_BASE + 0x18u)
#define LP_WDT_FLASHBOOT_EN (1u << 12)

/* The *super* watchdog, which is a third one and behaves differently: it
 * cannot be disabled, only set to feed itself. ESP-IDF's
 * bootloader_super_wdt_auto_feed() does exactly this and nothing else.
 *
 * It is here because leaving it out does not stop a board booting -- it stops
 * one *staying* booted. With the two flashboot watchdogs disarmed the kernel
 * came up cleanly and survived an eight-second look, then reset somewhere in
 * the following minute: long enough for a hardware suite to get twelve tests
 * in and blame the thirteenth. Its period is far longer than a boot, so any
 * test short enough to watch by hand passes. */
#define LP_WDT_SWD_CONFIG   (LP_WDT_BASE + 0x1cu)
#define LP_WDT_SWD_WPROTECT (LP_WDT_BASE + 0x20u)
#define LP_WDT_SWD_AUTO_FEED (1u << 18)
#define SWD_WKEY            0x50D83AA1u

#define TIMG0_BASE          0x500C2000u     /* HPPERIPH1 + 0x2000 */
#define TIMG0_WDTCONFIG0    (TIMG0_BASE + 0x48u)
#define TIMG0_WDTWPROTECT   (TIMG0_BASE + 0x64u)
#define TIMG0_FLASHBOOT_EN  (1u << 14)

/* The same value for both, from ESP-IDF's lpwdt_ll.h and mwdt_ll.h. */
#define WDT_WKEY            0x50D83AA1u

BOOT_TEXT void esp32p4_boot_wdt_disable(void) {
    volatile uint32_t *p;

    /* RWDT first, then MWDT0 -- IDF's order, so that a board is never
     * unprotected in between. */
    p = (volatile uint32_t *)(uintptr_t)LP_WDT_WPROTECT;  *p = WDT_WKEY;
    p = (volatile uint32_t *)(uintptr_t)LP_WDT_CONFIG0;   *p &= ~LP_WDT_FLASHBOOT_EN;
    p = (volatile uint32_t *)(uintptr_t)LP_WDT_WPROTECT;  *p = 0u;

    p = (volatile uint32_t *)(uintptr_t)TIMG0_WDTWPROTECT; *p = WDT_WKEY;
    p = (volatile uint32_t *)(uintptr_t)TIMG0_WDTCONFIG0;  *p &= ~TIMG0_FLASHBOOT_EN;
    p = (volatile uint32_t *)(uintptr_t)TIMG0_WDTWPROTECT; *p = 0u;

    /* And the super watchdog, which is told to feed itself rather than
     * turned off -- there is no off. */
    p = (volatile uint32_t *)(uintptr_t)LP_WDT_SWD_WPROTECT; *p = SWD_WKEY;
    p = (volatile uint32_t *)(uintptr_t)LP_WDT_SWD_CONFIG;   *p |= LP_WDT_SWD_AUTO_FEED;
    p = (volatile uint32_t *)(uintptr_t)LP_WDT_SWD_WPROTECT; *p = 0u;
}

typedef void (*rom_cache_op_t)(void);
typedef void (*rom_cache_map_op_t)(uint32_t map);
typedef int  (*rom_mmu_set_t)(uint32_t sensitive, uint32_t vaddr, uint32_t paddr,
                              uint32_t psize, uint32_t num, uint32_t fixed);

/* Points the flash XIP window at the OS image and turns the caches back on.
 *
 * Called from entry.S the instant a stack exists and before anything else,
 * because everything else is in flash.
 *
 * The order is ESP-IDF's own, from bootloader_utility.c's
 * set_cache_and_start_app(): disable the caches, change the mapping,
 * invalidate, re-enable. Changing a mapping under a live cache would leave
 * lines that answer for the old one, and the symptom of that is instructions
 * from whatever used to be at an address -- which on a board that has just
 * been reflashed is indistinguishable from a corrupt image.
 *
 * Returns nothing and checks nothing: there is no way to report a failure
 * from here. What makes that acceptable is that the failure is not silent
 * either -- a wrong mapping means the jump into flash fetches garbage, and
 * the board visibly does not reach a console. The diagnosis lives in
 * plan/phase32_esp32p4_execute_in_place.md, and the recovery is the ROM's
 * download mode, which no flash content can take away. */
BOOT_TEXT void esp32p4_xip_map(void) {
    rom_cache_op_t     dis_l2 = (rom_cache_op_t)(uintptr_t)ROM_CACHE_DISABLE_L2_CACHE;
    rom_cache_op_t     en_l2  = (rom_cache_op_t)(uintptr_t)ROM_CACHE_ENABLE_L2_CACHE;
    rom_cache_map_op_t inval  = (rom_cache_map_op_t)(uintptr_t)ROM_CACHE_INVALIDATE_ALL;
    rom_mmu_set_t      mmu    = (rom_mmu_set_t)(uintptr_t)ROM_CACHE_FLASH_MMU_SET;

    /* What to invalidate, and what emphatically not to.
     *
     * **Not the L1 data cache.** Invalidate discards; it does not write back.
     * This code's own stack is in L2MEM and reached through the L1 dcache, so
     * invalidating it throws away live stack frames -- including the return
     * address of the function doing it. Measured: with the dcache in this
     * mask the trace printed 'ABC00000000n00000004' and then watchdog-reset,
     * dying in the invalidate with the mapping already successfully set.
     *
     * L2 is the one that matters: it caches external memory, and its
     * contents describe the mapping that just changed. The L1 instruction
     * cache is included because it is harmless -- an icache holds no dirty
     * data, so invalidating only costs a refetch -- and because it is the
     * one place a stale line for 0x40000000 could otherwise hide. */
    const uint32_t inval_map = CACHE_MAP_L1_ICACHE_0 | CACHE_MAP_L2_CACHE;

    /* **Only L2.** Disabling the L1 caches here is fatal, and the reason is
     * the same fact phase 28 Z2 is built on: on this part internal memory is
     * reached *through* the L1 cache (SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE).
     * This code is executing from L2MEM, so turning off the L1 instruction
     * cache stops instruction fetch for the very code doing the turning off.
     * Measured: it printed 'X1' and then watchdog-reset in a loop, with the
     * next character never reaching the FIFO.
     *
     * ESP-IDF says the same thing in the form of an argument rather than a
     * comment -- bootloader_utility.c calls
     * `cache_hal_disable(CACHE_LL_LEVEL_EXT_MEM, ...)`, and EXT_MEM is level
     * 2. The L2 is what caches flash, and the L2 is all that has to stop. */
    dis_l2();

    /* vaddr and paddr are both 64 KB-aligned, which is the MMU's real
     * constraint (paddr % 64KB == vaddr % 64KB). ESP-IDF offsets its own
     * mapping by 0x20 to satisfy that around its image headers; we map a raw
     * binary and need no such adjustment. `fixed` is 0: consecutive virtual
     * pages follow consecutive physical ones. */
    (void)mmu(0u, 0x40000000u, (uint32_t)LUGALOS_P4_OSIMAGE_BASE,
              MMU_PAGE_KB, (uint32_t)(uintptr_t)_xip_pages, 0u);

    /* Invalidating L1 is fine and disabling it is not: an invalidate only
     * costs a refetch, where a disable stops the fetch. */
    inval(inval_map);

    en_l2();
}


/* Make instructions the kernel just *wrote* visible to instruction fetch.
 *
 * `fence.i` alone is not enough on this chip, and the ELF loader's comment
 * ("the kernel just wrote instructions through the data path") describes a
 * hazard that RISC-V's own instruction does not fully close here. Internal
 * memory is reached *through* L1 (SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE), and
 * L1 data is **writeback**: the loader's stores can still be sitting dirty in
 * the D-cache while the fetch of those same addresses misses I-cache, goes to
 * L2/RAM, and reads whatever was there before.
 *
 * Measured on the board, 2026-09-15: `exec` of the same binary faulted
 * differently on consecutive attempts -- `cause 2, epc=entry+4,
 * tval=0x269c790b` once and `cause 7, epc=entry+0, tval=0x1000` the next.
 * Same program, same address, different garbage, which is stale memory rather
 * than a bad image or a wrong entry offset.
 *
 * Order matters and is not symmetric. Write back **first**, so the loader's
 * bytes actually reach memory, and only then invalidate -- and invalidate the
 * *instruction* cache only. Phase 32 U2 established the other half of this
 * the hard way: invalidating L1 D discards dirty lines rather than writing
 * them back, which threw away the live stack and killed the board. After a
 * writeback-all nothing is dirty, so this is safe either way; naming just the
 * I-cache keeps it safe for the wrong reason as well as the right one. */
void esp32p4_icache_sync(void) {
    rom_cache_op_t     wb    = (rom_cache_op_t)(uintptr_t)ROM_CACHE_WRITEBACK_ALL;
    rom_cache_map_op_t inval = (rom_cache_map_op_t)(uintptr_t)ROM_CACHE_INVALIDATE_ALL;
    wb();
    inval(CACHE_MAP_L1_ICACHE_0);
}

#endif /* CONFIG_BOARD_ESP32P4 */
