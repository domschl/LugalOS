#ifndef LUGALOS_ARCH_VMM_H
#define LUGALOS_ARCH_VMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

typedef struct vmm_space {
#if defined(CONFIG_MMU)
    uintptr_t *page_table_root;
#endif
    uintptr_t heap_start;
    uintptr_t heap_end;
} vmm_space_t;

void vmm_init(void);
int vmm_map_page(vmm_space_t *space, uintptr_t vaddr, uintptr_t paddr, uint32_t flags);
void vmm_switch_space(vmm_space_t *space);
void *vmm_alloc_page(void);

#endif /* LUGALOS_ARCH_VMM_H */
