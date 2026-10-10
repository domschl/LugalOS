/* L2MEM layout for ESP32-P4 silicon revision v3.0 and later -- the
 * ESP32-P4-WIFI6-Touch-LCD-7B's v3.2. INCLUDEd by linker/esp32p4.ld; chosen by
 * CMake from CONFIG_ESP32P4_REV. 47.1, plan/phase47_esp32p4_lcd7b_ribbon.md.
 *
 * ## What v3 changed
 *
 * Both of the things v1.3 keeps out of the kernel's way swapped ends of
 * L2MEM (IDF: bootloader.memory.ld.in and esp_system/ld/esp32p4/memory.ld.in,
 * the !CONFIG_ESP32P4_SELECTS_REV_LESS_V3 branches):
 *
 *                 L2 cache storage                ROM data + download buffers
 *   rev < v3.0    top:    0x4ffc0000 - size ..    0x4ff296b8 .. 0x4ff40000
 *   rev >= v3.0   bottom: 0x4ff00000 .. + size    0x4ffa96b8 .. 0x4ffc0000
 *
 * **Measured on this board, 47.1:**
 *
 *   - The ROM leaves the cache at **128 KB**, not v1.3's 256 KB:
 *     CACHESIZE_CONF reads 0x200 before esp32p4_l2_cache_shrink() runs
 *     (the `[L2]` boot line). So 0x4ff20000 up is RAM from the first
 *     instruction, and even loadable sections may start there.
 *   - Using the v1.3 layout here put .bss at 0x4ff00200 -- inside the cache.
 *     It booted, ran the shell, and passed a marker test; then a build that
 *     moved .bss by a few hundred bytes put the page allocator's bitmap
 *     (g_fast_bitmap, 0x4ff03904) on a line the cache was using, and the
 *     allocator handed out the uart task's live stack. PAlloc's own overlap
 *     check caught it. The same E7 failure as v1.3's, at the other end.
 *
 * ## The layout
 *
 *   0x4ff00000 .. 0x4ff20000   L2 cache (CONFIG_L2_CACHE_KB = 128)
 *   0x4ff20000 .. 0x4ff5f000   LOWRAM: .bss, user stacks, boot stacks (NOLOAD)
 *   0x4ff5f000 .. 0x4ffa9000   RAM: .boot, .ramfunc, .utext, .data, then heap
 *   0x4ffa9000 .. 0x4ffc0000   left to the ROM (its data from 0x4ffa96b8)
 *
 * LOWRAM keeps v1.3's 252 KB so .bss and the stacks fit exactly as they do
 * on the NANO, and RAM gets what is left: 296 KB against the NANO's 384, so
 * about 85 KB less heap. That is the price of v3 putting the ROM's data in
 * the *middle* of what used to be free (v1.3 lets the heap run to the cache
 * at 0x4ffa0000; here the ROM starts at 0x4ffa96b8 and the cache costs its
 * 128 KB at the bottom instead). The download buffers (0x4ffa96b8 ..
 * 0x4ffbafc0) are dead after a flash boot and could be reclaimed, but core 1
 * starts on the ROM's CPU1 stack just above them; that is for a later
 * milestone to measure, not for this file to assume. */

MEMORY
{
    LOWRAM (rw)  : ORIGIN = 0x4ff20000, LENGTH = 252K
    RAM    (rwx) : ORIGIN = 0x4ff5f000, LENGTH = 296K
}

/* The bounds linker/esp32p4.ld asserts against. */
P4_LOAD_FLOOR  = 0x4ff20000;                         /* the ROM's 128 KB cache ends here at boot */
P4_LOWRAM_CEIL = 0x4ffa96b8;                         /* the ROM's data begins */
P4_RAM_FLOOR   = 0x4ff00000 + CONFIG_L2_CACHE_BYTES; /* the cache is the bottom of L2MEM */
P4_RAM_CEIL    = 0x4ffa96b8;
/* 296 KB of RAM less ~9 KB of loadable sections; the floor leaves the same
 * ~12 KB of slack for .boot/.utext/.data growth that v1.3's 360 has. */
P4_HEAP_FLOOR  = 275K;
