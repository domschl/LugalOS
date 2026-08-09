#ifndef LUGALOS_KERNEL_PALLOC_H
#define LUGALOS_KERNEL_PALLOC_H

#include <stdint.h>

/* Page allocator (B2, plan/phase5_distributed_design.md §5.4).
 *
 * B2 needs per-task kernel stacks, which means memory that can be handed out
 * *and given back*. What existed before could do neither safely: each arch's
 * vmm_alloc_page() was
 *
 *     void *ptr = (void *)current_heap; current_heap += PAGE_SIZE; return ptr;
 *
 * -- a bump pointer with no free, and, more seriously, **no upper bound**. On
 * RP2350 (512 KB SRAM, boot stack in SCRATCH_Y at the top) enough allocations
 * would have walked straight through the scratch regions and the boot stack
 * with no diagnostic. The linker scripts now export `_heap_end` so the bound
 * is a property of each board's memory map rather than a hope.
 *
 * Deliberately a *page* allocator, not a malloc: everything B2 needs is
 * page-granular (task stacks, and later page tables for B5's Sv39), and a
 * bitmap over a fixed page range is small enough to be obviously correct.
 * A general heap can come later if something actually needs one.
 *
 * Rule 0 (§5.1): identical on both builds. NOMMU has no reason to allocate
 * differently from MMU, and once B5's Sv39 needs page tables it will draw
 * from exactly this allocator.
 *
 * No locking: still single-call-stack until B2's scheduler lands, and
 * cooperative scheduling means allocation is never preempted mid-update.
 * Timer preemption (B6) must revisit that.
 */

/* arch/vmm.h defines this too; identical value, guarded so either include
 * order works. */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* Bounds the bitmap, and therefore how much of the heap is managed. RP2350
 * has ~400 KB of usable SRAM above the kernel image; QEMU has 128 MB, far
 * more than anything here needs, so it is capped rather than fully mapped. */
#if defined(CONFIG_BOARD_RP2350)
#define PALLOC_MAX_PAGES 128
#else
#define PALLOC_MAX_PAGES 4096
#endif

/* `start`/`end` are rounded inward to page boundaries. Managing more than
 * PALLOC_MAX_PAGES is clamped (and logged) rather than silently truncated. */
void palloc_init(uintptr_t start, uintptr_t end);

/* `n` contiguous pages, zeroed. NULL if no run of that size is free. */
void *palloc_pages(uint32_t n);
void  palloc_free(void *p, uint32_t n);

void palloc_stats(uint32_t *total_pages, uint32_t *free_pages);

#endif /* LUGALOS_KERNEL_PALLOC_H */
