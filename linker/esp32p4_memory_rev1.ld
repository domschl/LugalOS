/* L2MEM layout for ESP32-P4 silicon revisions below v3.0 -- the NANO's v1.3.
 * INCLUDEd by linker/esp32p4.ld (47.1, plan/phase47_esp32p4_lcd7b_ribbon.md);
 * the blocks below are unchanged from when they lived there.
 *
 * On this revision the ROM's data and download buffers sit LOW in L2MEM
 * (0x4ff296b8..0x4ff40000) and the L2 cache HIGH (the top
 * CONFIG_L2_CACHE_BYTES, 256 KB as the ROM leaves it). */

MEMORY
{
    /* NOLOAD SECTIONS ONLY. See the header comment: this range overlaps the
     * ROM's download-mode buffers, so anything the loader has to write into
     * it would corrupt the loader that is doing the writing. ld enforces the
     * *size*; the ASSERTs at the bottom enforce the rule.
     *
     * 252K, not 256K, and the missing 4 KB is the whole point (E6).
     *
     * E2 read the ROM's map out of IDF's own bootloader.memory.ld.in and
     * recorded `0x4ff3fba4`-`0x4ff40000` as "ROM .bss/.data", then placed the
     * boot stack across it anyway, reasoning that NOLOAD sections are safe in
     * the ROM's area because the ROM's buffers only matter while the loader
     * is writing. That is true of the *download buffers* -- region 1 of that
     * appendix -- and false of regions 4 and 5, which it labels "shared
     * memory used in startup code **and when IDF runs**" and "the 'interface'
     * data with constant addresses".
     *
     * It cost nothing until E6 called a ROM routine. `rom_spiflash_legacy_data`
     * and `_funcs` -- the pointers every ROM flash function dereferences --
     * live at 0x4ff3ffe8 and 0x4ff3ffec on this chip revision, inside the
     * boot stack, which entry.S paints with 0xA5A5A5A5. The first call into
     * ROM flash code faulted at `epc` inside the ROM, dereferencing
     * 0xa5a5a5a5: we had painted over the ROM's own function table.
     *
     * Note the revision dependency, because it is a trap. IDF ships two ROM
     * symbol files and this project uses the `eco0_4` one (chip v1.3). The
     * flash *function* addresses are identical in both; the *data* addresses
     * differ by 0x80000 -- 0x4ff3ffe8 here against 0x4ffbffe8 for r > ECO4.
     * Checking the functions and generalising would have given the wrong
     * answer with every appearance of having been verified.
     *
     * Shortening the region is what makes this structural: nothing the linker
     * can place is able to reach the reserved bytes, rather than nothing
     * happening to. 4 KB rather than the 1116 bytes strictly needed, because
     * a page is a rounder thing to reason about and LOWRAM has room. */
    LOWRAM (rw)  : ORIGIN = 0x4ff00000, LENGTH = 252K

    /* Everything loadable, and the heap after it.
     *
     * 384K, not the 512K that reaches L2MEM's end at 0x4ffc0000, and the
     * difference is the whole of E7's memory corruption.
     *
     * The L2 cache on this part is carved out of L2MEM itself, at the top, so
     * whatever is reserved for it is not RAM. The boot ROM leaves it at 256 KB
     * -- measured, by painting the heap and watching where the paint stopped
     * surviving: everything from 0x4ff80000 up decayed to zero, with no store
     * to blame and the page allocator provably innocent. ESP-IDF's own layout
     * states the relationship, for a chip below v3 as this v1.3 part is
     * (components/esp_system/ld/esp32p4/memory.ld.in):
     *
     *     #define SRAM_HIGH_START  0x4FF40000
     *     #define SRAM_HIGH_SIZE   0x80000 - CONFIG_CACHE_L2_CACHE_SIZE
     *
     * kernel_main() calls esp32p4_l2_cache_shrink() before touching the heap,
     * which asks the ROM for a 128 KB cache -- IDF's own default -- moving the
     * boundary to 0x4ffa0000. This length and that call are one change: the
     * ASSERT below is what keeps them from drifting apart. */
    RAM    (rwx) : ORIGIN = 0x4ff40000, LENGTH = 384K
}

/* The bounds linker/esp32p4.ld asserts against. */
P4_LOAD_FLOOR  = 0x4ff40000;                       /* above the ROM's download buffers */
P4_LOWRAM_CEIL = 0x4ff3f000;                       /* below the ROM's constant-address data */
P4_RAM_FLOOR   = 0x4ff00000;                       /* nothing below: L2MEM starts here */
P4_RAM_CEIL    = 0x4ffc0000 - CONFIG_L2_CACHE_BYTES; /* the cache is the top of L2MEM */
P4_HEAP_FLOOR  = 360K;                             /* see the ASSERT in linker/esp32p4.ld */
