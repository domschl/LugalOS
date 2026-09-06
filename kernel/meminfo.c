#include "kernel/meminfo.h"

/* See kernel/include/kernel/meminfo.h for the rationale.
 *
 * Everything here is derived from linker symbols. Nothing in this file knows
 * a board's addresses or sizes -- that is the point: the numbers /proc/meminfo
 * reports have to come from the same memory map the image was actually linked
 * against, or they are just a comment that compiles. */

extern char _ram_start[];
extern char _ram_end[];
extern char _bss_end[];
extern char _stack_bottom[];
extern char _stack_top[];
extern char _kernel_end[];
extern char _heap_end[];
extern char _data_start[];
extern char _data_end[];
extern char _bss_start[];

/* The poison as a full machine word. Built from STACK_POISON rather than
 * written out again, so the paint in entry.S and the scan here cannot
 * disagree. The 32-bit shift appears only in the branch where it is defined
 * behaviour -- on RV32 `(uintptr_t)x << 32` would be UB, and this build runs
 * UBSan on the QEMU targets. */
/* STACK_POISON_WORD now lives in kernel/meminfo.h, shared with sched.c's
 * per-task scan (§6). */
#define POISON_WORD STACK_POISON_WORD

uint32_t stack_used_bytes(void) {
    const uintptr_t *p   = (const uintptr_t *)(const void *)_stack_bottom;
    const uintptr_t *top = (const uintptr_t *)(const void *)_stack_top;

    /* Scan up from the bottom: the first word still carrying the pattern is
     * the deepest point never written. Reading upward from the low address
     * means the loop stops as soon as it finds live data, so the common case
     * (a shallow stack) walks nearly the whole region -- which is fine at a
     * few tens of KB on a read of /proc/meminfo, and is the direction that
     * keeps the answer a true high-water mark rather than a current depth. */
    while (p < top && *p == POISON_WORD) p++;

    return (uint32_t)((uintptr_t)top - (uintptr_t)p);
}

uint32_t stack_size_bytes(void) {
    return (uint32_t)((uintptr_t)_stack_top - (uintptr_t)_stack_bottom);
}

void meminfo_ram_map(mem_ram_map_t *out) {
    if (!out) return;

    out->ram_start    = (uintptr_t)_ram_start;
    out->ram_end      = (uintptr_t)_ram_end;
    out->total_bytes  = (uint32_t)((uintptr_t)_ram_end - (uintptr_t)_ram_start);
#if defined(CONFIG_BOARD_ESP32P4)
    /* This image is not one contiguous run from the bottom of RAM, which is
     * what `_bss_end - _ram_start` assumes everywhere else.
     *
     * linker/esp32p4.ld splits it: .bss and the boot stack sit in the low
     * 256 KB of L2MEM (safe there only because they are NOLOAD -- the ROM's
     * download-mode buffers overlap that range), while .text, .rodata and
     * .data are loaded into the 512 KB above them. So the two halves are
     * measured separately and added. Without this /proc/meminfo would report
     * the .bss half alone and silently omit ~227 KB of resident kernel text,
     * on the one target where the text IS resident -- and reporting a third
     * of an image as the whole of it is exactly the sort of number this file
     * exists to avoid. */
    {
        extern char _text_start[];
        out->image_bytes = (uint32_t)(((uintptr_t)_bss_end - (uintptr_t)_ram_start) +
                                      ((uintptr_t)_kernel_end - (uintptr_t)_text_start));
    }
#else
    out->image_bytes  = (uint32_t)((uintptr_t)_bss_end - (uintptr_t)_ram_start);
#endif
    out->stack_bytes  = stack_size_bytes();

    out->data_bytes = (uint32_t)((uintptr_t)_data_end - (uintptr_t)_data_start);
    out->bss_bytes  = (uint32_t)((uintptr_t)_bss_end - (uintptr_t)_bss_start);
#if defined(CONFIG_BOARD_RP2350) || defined(CONFIG_BOARD_ESP32P4)
    /* Both boards carry U-mode task stacks in their own NAPOT-aligned
     * sections, which are neither .data nor .bss and were invisible in the
     * image figure until they were reported here. The ESP32-P4 gained one in
     * E5 (plan/phase27_esp32p4_bringup.md). */
    {
        extern char _ustacks_start[];
        extern char _ustacks_end[];
        out->ustacks_bytes = (uint32_t)((uintptr_t)_ustacks_end -
                                        (uintptr_t)_ustacks_start);
    }
#else
    out->ustacks_bytes = 0;
#endif

    /* Exactly the range kernel_main() hands to palloc_init(), so the heap
     * line here and the page counts from palloc always describe the same
     * memory. Guarded because a linker script could in principle place
     * _kernel_end above _heap_end and leave no heap at all; palloc_init()
     * already survives that, and this should report it as zero rather than
     * underflow into a nonsense figure. */
    out->heap_bytes = (uintptr_t)_heap_end > (uintptr_t)_kernel_end
        ? (uint32_t)((uintptr_t)_heap_end - (uintptr_t)_kernel_end)
        : 0u;
}

bool meminfo_flash(uint32_t *used, uint32_t *total) {
#if defined(CONFIG_BOARD_RP2350)
    extern char __flash_binary_end[];
    extern char _flash_start[];
    extern char _flash_end[];

    if (used) {
        *used = (uint32_t)((uintptr_t)__flash_binary_end - (uintptr_t)_flash_start);
    }
    if (total) {
        *total = (uint32_t)((uintptr_t)_flash_end - (uintptr_t)_flash_start);
    }
    return true;
#else
    /* The QEMU targets link everything into one RAM region and are loaded
     * there directly -- there is no flash to report, and reporting the image
     * size again under a "Flash" heading would be actively misleading. */
    (void)used;
    (void)total;
    return false;
#endif
}
