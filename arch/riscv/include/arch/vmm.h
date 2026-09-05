#ifndef LUGALOS_ARCH_VMM_H
#define LUGALOS_ARCH_VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PAGE_SIZE 4096

typedef struct vmm_space {
#if defined(CONFIG_MMU)
    uintptr_t *page_table_root;
#endif
    uintptr_t heap_start;
    uintptr_t heap_end;
} vmm_space_t;

void vmm_init(void);

/* Joins a secondary hart to the kernel address space vmm_init() built.
 * satp is per-hart, so without this a secondary runs in bare mode while the
 * primary translates -- see the definition. No-op where paging is off. */
void vmm_secondary_init(void);
int vmm_map_page(vmm_space_t *space, uintptr_t vaddr, uintptr_t paddr, uint32_t flags);
void vmm_switch_space(vmm_space_t *space);
void *vmm_alloc_page(void);

/* --- Sv39 (B5, MMU builds only; no-ops or absent on NOMMU) --- */

/* Maps [va, va+size) using the largest superpage each alignment allows.
 * `leaf_flags` are raw Sv39 PTE permission bits. Returns 0, or -1. */
int vmm_map_range(uintptr_t *root, uintptr_t va, uintptr_t pa, uintptr_t size,
                  uintptr_t leaf_flags);

/* The kernel's root page table, for building a task space that shares it. */
uintptr_t *vmm_kernel_root(void);

/* Returns a page-table tree built for a memory domain to the allocator (C2).
 * Frees only the tables, never the memory they map; see the Sv39
 * implementation for why that distinction is the R/W/X bits. A no-op on the
 * NOMMU backend, which has no tables -- callers do not branch on the memory
 * model (Rule 0, plan/phase5_distributed_design.md §5.1). */
void vmm_free_table(uintptr_t *root);

/* False if paging could not be brought up, so callers can report honestly
 * rather than claim enforcement that is not there. */
bool vmm_paging_enabled(void);

#endif /* LUGALOS_ARCH_VMM_H */
