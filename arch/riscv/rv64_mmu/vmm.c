#include "arch/vmm.h"
#include "kernel/palloc.h"
#include "arch/csr.h"
#include "kernel/printk.h"
#include <string.h>

/* Sv39 paging (B5, plan/phase5_distributed_design.md §5.4).
 *
 * What this replaces was a stub in every way that mattered: vmm_map_page()
 * was `return 0;`, satp was never written, and vmm_switch_space() was never
 * called. RV64 and RV32 were both effectively flat -- which §5.1's Rule 3
 * predicted would happen to an MMU backend nothing exercised, and which is
 * why B3 put enforcement on the M-mode targets first.
 *
 * B5 implements the interface B3 already defined and tested (mem_domain_t),
 * so the MMU backend arrives against a contract with a working reference
 * implementation rather than inventing one.
 *
 * ## Sv39 in brief
 *
 * 39-bit virtual addresses, three levels of 512-entry tables, 4 KB pages.
 * VA[38:30] indexes the root (L2), VA[29:21] the next (L1), VA[20:12] the
 * leaf (L0). A table entry with none of R/W/X set is a pointer to the next
 * level; one with any of them set is a leaf, and a leaf at L2 or L1 is a
 * 1 GB or 2 MB "superpage". Superpages matter here: mapping 128 MB of RAM as
 * 4 KB pages would cost 32768 PTEs, while two 1 GB leaves cost two.
 */

extern char _kernel_end[];
static uintptr_t *kernel_page_table;
static bool g_paging_on;

/* Sv39 PTE flags */
#define PTE_V (1UL << 0)
#define PTE_R (1UL << 1)
#define PTE_W (1UL << 2)
#define PTE_X (1UL << 3)
#define PTE_U (1UL << 4)
#define PTE_G (1UL << 5)
#define PTE_A (1UL << 6)
#define PTE_D (1UL << 7)

#define SATP_MODE_SV39 (8UL << 60)

#define SSTATUS_SUM (1UL << 18)

#define LEVELS      3
#define PTE_PER_TAB 512

static inline uintptr_t pte_from_pa(uintptr_t pa, uintptr_t flags) {
    return ((pa >> 12) << 10) | flags;
}
static inline uintptr_t pa_from_pte(uintptr_t pte) {
    return (pte >> 10) << 12;
}
static inline uint32_t vpn(uintptr_t va, int level) {
    return (uint32_t)((va >> (12 + 9 * level)) & 0x1ff);
}

/* Page size covered by one entry at `level`: 4 KB, 2 MB, 1 GB. */
static inline uintptr_t level_size(int level) {
    return 1UL << (12 + 9 * level);
}

/* Maps [va, va+size) to [pa, ...) with `leaf_flags`, choosing the largest
 * superpage that both addresses and the remaining length can support. Returns
 * 0, or -1 if a table could not be allocated. */
int vmm_map_range(uintptr_t *root, uintptr_t va, uintptr_t pa, uintptr_t size,
                  uintptr_t leaf_flags) {
    if (!root) return -1;
    uintptr_t end = va + size;

    while (va < end) {
        int level = LEVELS - 1;
        /* Descend to the largest level whose page fits and is aligned. */
        while (level > 0) {
            uintptr_t sz = level_size(level);
            if ((va % sz) == 0 && (pa % sz) == 0 && (end - va) >= sz) break;
            level--;
        }

        uintptr_t *tab = root;
        for (int l = LEVELS - 1; l > level; l--) {
            uintptr_t *slot = &tab[vpn(va, l)];
            if (!(*slot & PTE_V)) {
                void *next = palloc_pages(1);
                if (!next) return -1;
                memset(next, 0, PAGE_SIZE);
                /* A pointer entry has no R/W/X -- that is what distinguishes
                 * it from a leaf, not a separate bit. */
                *slot = pte_from_pa((uintptr_t)next, PTE_V);
            } else if (*slot & (PTE_R | PTE_W | PTE_X)) {
                /* A superpage already covers this address. Splitting it is
                 * not attempted: nothing here remaps a range it already
                 * mapped, and silently shrinking someone else's mapping is
                 * worse than refusing. */
                return -1;
            }
            tab = (uintptr_t *)pa_from_pte(*slot);
        }

        /* A/D set explicitly: the spec permits an implementation to fault
         * rather than set them in hardware, and there is nothing useful to do
         * in that fault handler here. */
        tab[vpn(va, level)] = pte_from_pa(pa, leaf_flags | PTE_V | PTE_A | PTE_D);

        uintptr_t step = level_size(level);
        va += step;
        pa += step;
    }
    return 0;
}

uintptr_t *vmm_kernel_root(void) { return kernel_page_table; }

void vmm_init(void) {
    kernel_page_table = (uintptr_t *)palloc_pages(1);
    if (!kernel_page_table) {
        printk("[VMM] No memory for the root page table; paging stays off\n");
        return;
    }
    memset(kernel_page_table, 0, PAGE_SIZE);

    /* Identity-map the low 4 GB as four 1 GB superpages: RWX, global, no U.
     *
     * Identity rather than a relocated kernel window, deliberately. Every
     * pointer already in flight when satp is written -- the stack, the return
     * address, the page table's own address -- keeps working, so enabling
     * paging is not observable. A relocating map would have to get all of
     * that right at the instant translation turns on, for no benefit this
     * kernel needs.
     *
     * Four gigapages covers QEMU virt's MMIO (CLINT, PLIC, UART at
     * 0x10000000, virtio) and its RAM at 0x80000000 without enumerating
     * either. PTE_G marks them global so they survive an ASID change.
     */
    if (vmm_map_range(kernel_page_table, 0, 0, 4UL * 1024 * 1024 * 1024,
                      PTE_R | PTE_W | PTE_X | PTE_G) != 0) {
        printk("[VMM] Kernel identity map failed; paging stays off\n");
        return;
    }

    /* SUM lets S-mode read and write pages marked U. copy_from_user() and
     * copy_to_user() do exactly that, so without this every syscall taking a
     * pointer would fault the moment U-mode pages exist. */
    set_csr(sstatus, SSTATUS_SUM);

    uintptr_t satp_val = SATP_MODE_SV39 | ((uintptr_t)kernel_page_table >> 12);
    write_csr(satp, satp_val);
    __asm__ __volatile__("sfence.vma zero, zero");
    g_paging_on = true;

    printk("[VMM] Sv39 paging enabled. Root page table at 0x%lx\n",
           (unsigned long)kernel_page_table);
}

bool vmm_paging_enabled(void) { return g_paging_on; }

void *vmm_alloc_page(void) {
    return palloc_pages(1);
}

int vmm_map_page(vmm_space_t *space, uintptr_t vaddr, uintptr_t paddr, uint32_t flags) {
    uintptr_t *root = (space && space->page_table_root) ? space->page_table_root
                                                        : kernel_page_table;
    return vmm_map_range(root, vaddr, paddr, PAGE_SIZE, (uintptr_t)flags);
}

void vmm_switch_space(vmm_space_t *space) {
    uintptr_t root = (uintptr_t)kernel_page_table;
    if (space && space->page_table_root) {
        root = (uintptr_t)space->page_table_root;
    }
    write_csr(satp, SATP_MODE_SV39 | (root >> 12));
    __asm__ __volatile__("sfence.vma zero, zero");
}
