#include "arch/vmm.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"

extern char _kernel_end[];
static uintptr_t current_heap;

void vmm_init(void) {
    current_heap = (uintptr_t)_kernel_end;
    /* Align heap pointer to page boundary */
    if (current_heap & (PAGE_SIZE - 1)) {
        current_heap = (current_heap + PAGE_SIZE) & ~(PAGE_SIZE - 1);
    }
    printk("[VMM] NOMMU Memory Manager initialized. Free heap starting at 0x%lx\n",
           (unsigned long)current_heap);
}

int vmm_map_page(vmm_space_t *space, uintptr_t vaddr, uintptr_t paddr, uint32_t flags) {
    (void)space;
    (void)flags;
    /* On NOMMU systems, virtual address equals physical address (identity mapping) */
    if (vaddr != paddr) {
        printk("[VMM Error] NOMMU target cannot re-map 0x%lx to 0x%lx\n",
               (unsigned long)vaddr, (unsigned long)paddr);
        return -1;
    }
    return 0;
}

void vmm_switch_space(vmm_space_t *space) {
    (void)space;
    /* No page table switching on NOMMU hardware */
}

/* B2: delegates to the one page allocator instead of bumping an unbounded
 * pointer. The old version could not free and could not fail -- it just kept
 * handing out addresses past the end of RAM. */
void *vmm_alloc_page(void) {
    return palloc_pages(1);
}
