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

/* Bounds the bitmap, and therefore how much of the heap is managed.
 *
 * 128 pages is 512 KB, the whole of RP2350's main SRAM -- deliberately more
 * than the heap can ever be, so this cap is never the thing that limits it.
 * The real limit there is what is left above _kernel_end, which as of this
 * writing is 60 KB (15 pages): the image's .data + .bss reach nearly to the
 * top of RAM. /proc/meminfo reports both figures, so the day one of them
 * moves it is visible rather than inferred. QEMU has 128 MB, far more than
 * anything here needs, so it is capped rather than fully mapped.
 *
 * Sourced from the generated per-board config (K1,
 * plan/phase7_kernel_config.md) rather than an #ifdef here -- the value
 * itself, and which board gets which value, live in cmake/board-*.cmake. */
#include "lugalos_config.h"
#define PALLOC_MAX_PAGES CONFIG_PALLOC_MAX_PAGES

/* `start`/`end` are rounded inward to page boundaries. Managing more than
 * PALLOC_MAX_PAGES is clamped (and logged) rather than silently truncated. */
void palloc_init(uintptr_t start, uintptr_t end);

/* `n` contiguous pages, zeroed. NULL if no run of that size is free. */
void *palloc_pages(uint32_t n);

/* As above, but the returned block also starts on an `align_pages` boundary
 * (`align_pages` must be a power of two; 1 means "page-aligned", i.e. the
 * same as palloc_pages()).
 *
 * This exists for a hardware reason rather than a stylistic one. A PMP region
 * must be power-of-two sized *and self-aligned* (NAPOT -- see
 * kernel/mem_domain.h), so a user image that wants its text page granted R|X
 * and its data page R|W needs the two pages to sit inside one naturally
 * aligned block: within a 2^k-aligned run, every page-sized piece is itself
 * page-aligned, and each piece is separately expressible. A merely contiguous
 * pair is not, and the loader would have had to either widen a grant or give
 * up on W^X.
 *
 * The MMU build does not need the alignment -- Sv39 maps at page granularity
 * regardless -- but takes it anyway, per Rule 0 (§5.1): one allocation rule,
 * so the NOMMU constraint is not something only one build has ever run. */
void *palloc_pages_aligned(uint32_t n, uint32_t align_pages);

void  palloc_free(void *p, uint32_t n);

void palloc_stats(uint32_t *total_pages, uint32_t *free_pages);

/* The two figures palloc_stats() cannot give:
 *
 *   peak_used_pages  -- high-water mark since palloc_init(), never reset. The
 *                       one number that says whether the heap is sized right;
 *                       free-page counts only ever describe this instant.
 *   largest_free_run -- longest run of contiguous free pages, so a
 *                       fragmentation failure is distinguishable from simply
 *                       running out.
 *
 * Either pointer may be NULL. */
void palloc_extra_stats(uint32_t *peak_used_pages, uint32_t *largest_free_run);

#endif /* LUGALOS_KERNEL_PALLOC_H */
