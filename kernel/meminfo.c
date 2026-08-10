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

/* The poison as a full machine word. Built from STACK_POISON rather than
 * written out again, so the paint in entry.S and the scan here cannot
 * disagree. The 32-bit shift appears only in the branch where it is defined
 * behaviour -- on RV32 `(uintptr_t)x << 32` would be UB, and this build runs
 * UBSan on the QEMU targets. */
#if UINTPTR_MAX > 0xFFFFFFFFu
#define POISON_WORD (((uintptr_t)STACK_POISON << 32) | (uintptr_t)STACK_POISON)
#else
#define POISON_WORD ((uintptr_t)STACK_POISON)
#endif

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
    out->image_bytes  = (uint32_t)((uintptr_t)_bss_end - (uintptr_t)_ram_start);
    out->stack_bytes  = stack_size_bytes();

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
