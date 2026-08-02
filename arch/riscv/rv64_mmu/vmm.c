#include "arch/vmm.h"
#include "arch/csr.h"
#include "kernel/printk.h"

extern char _kernel_end[];
static uintptr_t current_heap;
static uintptr_t *kernel_page_table;

/* Sv39 PTE Flags */
#define PTE_V (1 << 0)
#define PTE_R (1 << 1)
#define PTE_W (1 << 2)
#define PTE_X (1 << 3)
#define PTE_U (1 << 4)
#define PTE_G (1 << 5)
#define PTE_A (1 << 6)
#define PTE_D (1 << 7)

#define SATP_MODE_SV39 (8ULL << 60)

void vmm_init(void) {
    current_heap = (uintptr_t)_kernel_end;
    if (current_heap & (PAGE_SIZE - 1)) {
        current_heap = (current_heap + PAGE_SIZE) & ~(PAGE_SIZE - 1);
    }
    
    /* Allocate L2 root page table */
    kernel_page_table = (uintptr_t *)vmm_alloc_page();
    for (int i = 0; i < 512; i++) {
        kernel_page_table[i] = 0;
    }

    printk("[VMM] Sv39 MMU Memory Manager initialized. Root page table at 0x%lx\n",
           (unsigned long)kernel_page_table);
}

void *vmm_alloc_page(void) {
    void *ptr = (void *)current_heap;
    current_heap += PAGE_SIZE;
    return ptr;
}

int vmm_map_page(vmm_space_t *space, uintptr_t vaddr, uintptr_t paddr, uint32_t flags) {
    (void)space;
    (void)vaddr;
    (void)paddr;
    (void)flags;
    /* Sv39 page table entry allocation stub */
    return 0;
}

void vmm_switch_space(vmm_space_t *space) {
    uintptr_t root = (uintptr_t)kernel_page_table;
    if (space && space->page_table_root) {
        root = (uintptr_t)space->page_table_root;
    }
    
    /* satp = (SATP_MODE_SV39) | (ppn) */
    uintptr_t ppn = root >> 12;
    uintptr_t satp_val = SATP_MODE_SV39 | ppn;
    
#if defined(CONFIG_MODE_S)
    write_csr(satp, satp_val);
    __asm__ __volatile__("sfence.vma zero, zero");
#endif
}
