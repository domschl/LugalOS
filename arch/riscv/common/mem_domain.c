#include "kernel/mem_domain.h"
#include "kernel/hart.h"
#include "kernel/printk.h"
#include "arch/csr.h"
#include "kernel/palloc.h"
#include <string.h>

/* See kernel/include/kernel/mem_domain.h for the rationale. */

void mem_domain_init(mem_domain_t *d) {
    if (d) memset(d, 0, sizeof(*d));
}

uint32_t g_domain_activations[MAX_HARTS];

uint32_t mem_domain_activations(unsigned hart) {
    return (hart < MAX_HARTS) ? g_domain_activations[hart] : 0;
}

int mem_domain_add(mem_domain_t *d, uintptr_t base, uintptr_t size, uint8_t perms) {
    if (!d || d->count >= MEM_DOMAIN_MAX_REGIONS) return -1;
    /* NAPOT requires a power-of-two size, self-aligned. The floor is 32 bytes
     * -- RP2350's granule (datasheet §3.8.3.1); QEMU's is 8, and taking the
     * stricter of the two keeps one rule for both builds rather than a region
     * that is legal on one target and silently widened on the other.
     * Rejected rather than rounded: a region quietly widened to satisfy
     * alignment is a hole in the isolation it was asked to provide. */
    if (size < 32 || (size & (size - 1)) != 0) return -1;
    if (base & (size - 1)) return -1;

    d->regions[d->count].base  = base;
    d->regions[d->count].size  = size;
    d->regions[d->count].perms = perms;
    d->count++;
    return 0;
}

/* Architecture-independent, and deliberately so: this is the check the
 * syscall boundary uses to decide whether a user-supplied pointer is one the
 * calling task may actually touch. It reads the region list, not the
 * hardware, so it behaves identically on both builds -- including on the
 * S-mode target where nothing is enforced yet, so that copy-in/copy-out is
 * exercised by the whole test suite rather than only where PMP exists.
 *
 * A NULL domain means an unrestricted (kernel) task, for which every address
 * is permitted -- kernel code calling a syscall on its own behalf. */
bool mem_domain_permits(const mem_domain_t *d, uintptr_t base, uintptr_t len,
                        uint8_t perms) {
    if (!d) return true;
    if (len == 0) return true;
    if (base + len < base) return false; /* wrap-around */

    /* The range must fall entirely inside ONE region. Stitching a range
     * together from several adjacent regions would be wrong: PMP resolves
     * each access against the lowest-numbered matching region, so a range
     * spanning two of them is not uniformly governed by either. */
    for (int i = 0; i < d->count; i++) {
        uintptr_t rb = d->regions[i].base;
        uintptr_t re = rb + d->regions[i].size;
        if (base >= rb && base + len <= re) {
            return (d->regions[i].perms & perms) == perms;
        }
    }
    return false;
}

#if defined(CONFIG_MODE_M)

/* Permission bit order differs by silicon -- this is erratum RP2350-E6, not a
 * choice. The RP2350 datasheet (§3.8.3.2) states:
 *
 *   "Due to RP2350-E6 the field order in the PMP configuration fields is
 *    R, W, X (MSB-first) rather than the standard X, W, R."
 *
 * So on RP2350 bit 0 is X and bit 2 is R -- the reverse of every other
 * RISC-V core, including QEMU's. Getting this wrong is not a subtle
 * degradation: a region asked for as R|W becomes X|W, which is why a U-mode
 * task's stores to its own stack succeeded while its loads from the same
 * stack faulted. That symptom cost a long detour through NAPOT encoding and
 * an abandoned attempt to use TOR before the datasheet named the cause. */
#if defined(CONFIG_BOARD_RP2350)
#define PMP_X       (1u << 0)
#define PMP_W       (1u << 1)
#define PMP_R       (1u << 2)
#else
#define PMP_R       (1u << 0)
#define PMP_W       (1u << 1)
#define PMP_X       (1u << 2)
#endif
#define PMP_A_NAPOT (3u << 3)

/* Granule: the smallest NAPOT region the hardware decodes. RP2350 hardwires
 * the low two pmpaddr bits to ones when a region is enabled, giving 8 << 2 =
 * 32 bytes (datasheet §3.8.3.1). Those bits are therefore not meaningful to
 * compare on readback. */
#if defined(CONFIG_BOARD_RP2350)
#define PMP_GRANULE       32u
#define PMP_ADDR_FIXED_LOW 3u   /* bits [1:0] hardwired */
#else
#define PMP_GRANULE       8u
#define PMP_ADDR_FIXED_LOW 0u
#endif

/* NAPOT only. The RP2350 datasheet (§3.8.3.1) is explicit: "The RP2350
 * configuration of Hazard3 supports only the OFF and NAPOT values for the
 * PMPCFG A fields." An earlier attempt here used TOR and every region came
 * back inactive -- writing A=TOR reads back as A=OFF, because the field is
 * WARL and the unsupported mode is simply dropped.
 *
 * Size is encoded as trailing one-bits: a value with k trailing ones matches
 * 8 << k bytes, naturally aligned. */

#define PMP_ENTRIES 8

#if defined(CONFIG_BOARD_RP2350)
/* Hazard3 hardwires three PMP regions that this kernel cannot switch off, and
 * they are not neutral: RP2350 datasheet §3.8.8.1 says regions 8, 9 and 10
 * "provide default U-mode RWX permissions" on
 *
 *     ROM          0x00000000 .. 0x0fffffff
 *     Peripherals  0x40000000 .. 0x5fffffff
 *     SIO          0xd0000000 .. 0xdfffffff
 *
 * So on this silicon a task confined to three pages of RAM still had read,
 * write *and execute* on every peripheral in the chip -- UART, SPI, PWM,
 * PADS, the watchdog, the TICKS block this kernel's own timer depends on --
 * plus the boot ROM and SIO. That is not a narrow leak; it is enough to
 * reprogram or reset the machine, and it would have made "the program is
 * confined" a false claim on the one target where the enforcement is real.
 *
 * Found by running the isolation probe on hardware rather than by reading
 * the datasheet first: on QEMU the same program faults, because QEMU's PMP
 * has no hardwired regions. It is the third time B3/B6 has hit a Hazard3
 * property invisible under emulation, after NAPOT-only and erratum E6.
 *
 * The fix is in the same paragraph of the datasheet: "The dynamic regions 0
 * through 7 take priority over the hardwired regions." Three dynamic regions
 * covering the same ranges with *no* permissions therefore revoke them. They
 * are installed for every domain, from the top of the entry table down, so
 * they cost nothing a domain would otherwise use and a domain that genuinely
 * needs a device window can still grant one -- its region is lower-numbered,
 * so it wins.
 *
 * Each range is already a naturally aligned power of two, which is what makes
 * this expressible at all: 256 MB, 512 MB, 256 MB. */
#define PMP_DENY_ENTRIES 3
#define PMP_DYNAMIC_ENTRIES (PMP_ENTRIES - PMP_DENY_ENTRIES)

static const struct { uintptr_t base, size; } g_hardwired_shadow[PMP_DENY_ENTRIES] = {
    { 0x00000000u, 0x10000000u },  /* ROM         */
    { 0x40000000u, 0x20000000u },  /* Peripherals */
    { 0xd0000000u, 0x10000000u },  /* SIO         */
};
#else
#define PMP_DENY_ENTRIES 0
#define PMP_DYNAMIC_ENTRIES PMP_ENTRIES
#endif

static void pmpaddr_write(int idx, uintptr_t v) {
    switch (idx) {
        case 0: write_csr(pmpaddr0, v); break;
        case 1: write_csr(pmpaddr1, v); break;
        case 2: write_csr(pmpaddr2, v); break;
        case 3: write_csr(pmpaddr3, v); break;
        case 4: write_csr(pmpaddr4, v); break;
        case 5: write_csr(pmpaddr5, v); break;
        case 6: write_csr(pmpaddr6, v); break;
        case 7: write_csr(pmpaddr7, v); break;
        default: break;
    }
}

static uintptr_t pmpaddr_read(int idx) {
    switch (idx) {
        case 0: return read_csr(pmpaddr0);
        case 1: return read_csr(pmpaddr1);
        case 2: return read_csr(pmpaddr2);
        case 3: return read_csr(pmpaddr3);
        case 4: return read_csr(pmpaddr4);
        case 5: return read_csr(pmpaddr5);
        case 6: return read_csr(pmpaddr6);
        case 7: return read_csr(pmpaddr7);
        default: return 0;
    }
}

/* NAPOT: base>>2 with the low (log2(size) - 3) bits set to ones. */
static uintptr_t napot_encode(uintptr_t base, uintptr_t size) {
    return (base >> 2) | ((size >> 3) - 1);
}

int mem_domain_activate(const mem_domain_t *d) {
    /* X2, plan/phase23_multicore_scheduling.md: which harts have actually
     * activated a restricted domain.
     *
     * The milestone asks whether per-task isolation still holds "regardless
     * of which hart activates a given domain". That question cannot be
     * answered by a test that merely passes -- with driver tasks pinned to
     * hart 0, a run could satisfy every isolation check while never once
     * activating a domain anywhere else. This counter is what turns the
     * claim into evidence: a non-zero entry for hart 1 means a restricted
     * domain was installed there, by code running there. */
    if (d) g_domain_activations[hart_id()]++;
    int n = d ? d->count : 0;
    if (n > MEM_DOMAIN_MAX_REGIONS) n = MEM_DOMAIN_MAX_REGIONS;
    if (n > PMP_DYNAMIC_ENTRIES) n = PMP_DYNAMIC_ENTRIES;

    uint8_t cfg[PMP_ENTRIES];
    for (int i = 0; i < PMP_ENTRIES; i++) cfg[i] = 0;
    for (int i = 0; i < n; i++) {
        uint8_t byte = PMP_A_NAPOT;
        if (d->regions[i].perms & MEM_R) byte |= PMP_R;
        if (d->regions[i].perms & MEM_W) byte |= PMP_W;
        if (d->regions[i].perms & MEM_X) byte |= PMP_X;
        cfg[i] = byte;
    }
#if PMP_DENY_ENTRIES > 0
    /* Active (A=NAPOT) with every permission bit clear: a match that grants
     * U-mode nothing, which is what overrides the hardwired grants above.
     * Installed only when there is a domain to enforce -- a kernel task runs
     * with no restriction at all, and M-mode ignores an unlocked region
     * anyway (datasheet §3.8.3.2). */
    if (d) {
        for (int i = 0; i < PMP_DENY_ENTRIES; i++) cfg[PMP_DYNAMIC_ENTRIES + i] = PMP_A_NAPOT;
    }
#endif

    /* Config first (see above), and every byte including the unused entries:
     * writing the whole word is what makes a switch to a smaller domain
     * actually shrink access rather than inherit the previous task's
     * leftovers -- the leak the hardware suite caught after `usertest`, made
     * structurally impossible. */
    uintptr_t w0 = 0, w1 = 0;
#if __riscv_xlen == 32
    for (int i = 0; i < 4; i++) w0 |= ((uintptr_t)cfg[i]) << (i * 8);
    for (int i = 0; i < 4; i++) w1 |= ((uintptr_t)cfg[4 + i]) << (i * 8);
    write_csr(pmpcfg0, w0);
    write_csr(pmpcfg1, w1);
#else
    for (int i = 0; i < 8; i++) w0 |= ((uintptr_t)cfg[i]) << (i * 8);
    write_csr(pmpcfg0, w0);
    (void)w1;
#endif

    int ok = 0;
#if PMP_DENY_ENTRIES > 0
    if (d) {
        for (int i = 0; i < PMP_DENY_ENTRIES; i++) {
            pmpaddr_write(PMP_DYNAMIC_ENTRIES + i,
                          napot_encode(g_hardwired_shadow[i].base,
                                       g_hardwired_shadow[i].size));
        }
    }
#endif
    for (int i = 0; i < n; i++) {
        uintptr_t want = napot_encode(d->regions[i].base, d->regions[i].size);
        pmpaddr_write(i, want);
        /* A region the hardware declined to represent exactly is not a
         * smaller region -- it is an unknown one. Report failure so the
         * caller can refuse to run a task under a restriction nobody has
         * verified, rather than granting something arbitrary. */
        /* Compare only the bits the hardware lets software own. RP2350
         * hardwires the low two to ones when the region is enabled (granule
         * 32), so they carry no information and differ by design. */
        uintptr_t got = pmpaddr_read(i);
        if ((got | PMP_ADDR_FIXED_LOW) != (want | PMP_ADDR_FIXED_LOW)) {
            printk("[MemDomain] pmpaddr%d: wrote %08lx, kept %08lx "
                   "(region %08lx+%08lx not representable on this core)\n",
                   i, (unsigned long)want, (unsigned long)got,
                   (unsigned long)d->regions[i].base,
                   (unsigned long)d->regions[i].size);
            ok = -1;
        }
    }
    return ok;
}

bool mem_domain_enforced(void) { return true; }

const char *mem_domain_backend_name(void) { return "PMP"; }

/* Nothing cached: a PMP domain is the region list itself, reprogrammed on
 * every context switch. Present so the loader can release a domain without
 * asking which memory model it is on (Rule 0, §5.1). */
void mem_domain_destroy(mem_domain_t *d) {
    if (!d) return;
    d->count = 0;
    d->arch_priv = NULL;
}

#else /* CONFIG_MODE_S -- Sv39 (B5) */

#include "arch/vmm.h"

#define PTE_R (1UL << 1)
#define PTE_W (1UL << 2)
#define PTE_X (1UL << 3)
#define PTE_U (1UL << 4)
#define PTE_G (1UL << 5)

#define MB2 (2UL * 1024 * 1024)
#define GB1 (1024UL * 1024 * 1024)

/* QEMU virt: MMIO below 1 GB, RAM at 0x80000000. Mapped separately because
 * only the RAM gigabyte ever contains user regions, and it is the only one
 * that needs splitting to finer granularity. */
#define RAM_BASE 0x80000000UL

static const mem_region_t *region_at(const mem_domain_t *d, uintptr_t addr) {
    for (int i = 0; i < d->count; i++) {
        if (addr >= d->regions[i].base &&
            addr < d->regions[i].base + d->regions[i].size) {
            return &d->regions[i];
        }
    }
    return NULL;
}

/* How a 2 MB block should be mapped. Splitting is expensive -- 512 PTEs and a
 * table -- so it is done only where a region boundary actually falls inside
 * the block. An earlier version split every block a region merely touched,
 * which turned the 128 MB text region into 32768 individual page mappings and
 * hung the boot. */
typedef enum { BLK_KERNEL, BLK_WHOLE_REGION, BLK_SPLIT } blk_kind_t;

static blk_kind_t classify_block(const mem_domain_t *d, uintptr_t base,
                                 uintptr_t size, const mem_region_t **whole) {
    bool partial = false;
    const mem_region_t *cover = NULL;

    for (int i = 0; i < d->count; i++) {
        uintptr_t rb = d->regions[i].base, re = rb + d->regions[i].size;
        if (rb >= base + size || re <= base) continue;      /* no overlap */
        if (rb <= base && re >= base + size) {
            /* Fully covers the block. First match wins, matching PMP's
             * lowest-numbered-region rule so both backends resolve overlaps
             * the same way. */
            if (!cover) cover = &d->regions[i];
        } else {
            partial = true;
        }
    }
    if (partial) return BLK_SPLIT;      /* a boundary lands inside this block */
    if (cover) { *whole = cover; return BLK_WHOLE_REGION; }
    return BLK_KERNEL;
}

static uintptr_t leaf_flags_for(const mem_region_t *r) {
    uintptr_t f = PTE_U;
    if (r->perms & MEM_R) f |= PTE_R;
    if (r->perms & MEM_W) f |= PTE_W;
    if (r->perms & MEM_X) f |= PTE_X;
    return f;
}

/* Builds this domain's address space: the kernel identity map (so the kernel
 * keeps working after a trap, which runs with the faulting task's satp still
 * active) plus the domain's regions marked U. */
static uintptr_t *build_space(const mem_domain_t *d) {
    uintptr_t *root = (uintptr_t *)palloc_pages(1);
    if (!root) return NULL;
    memset(root, 0, PAGE_SIZE);

    const uintptr_t kflags = PTE_R | PTE_W | PTE_X | PTE_G;

    /* MMIO gigabyte, as a single superpage. */
    if (vmm_map_range(root, 0, 0, GB1, kflags) != 0) return NULL;

    for (uintptr_t blk = RAM_BASE; blk < RAM_BASE + GB1; blk += MB2) {
        const mem_region_t *whole = NULL;
        switch (classify_block(d, blk, MB2, &whole)) {
            case BLK_KERNEL:
                if (vmm_map_range(root, blk, blk, MB2, kflags) != 0) return NULL;
                break;
            case BLK_WHOLE_REGION:
                if (vmm_map_range(root, blk, blk, MB2, leaf_flags_for(whole)) != 0) return NULL;
                break;
            case BLK_SPLIT:
                for (uintptr_t pg = blk; pg < blk + MB2; pg += PAGE_SIZE) {
                    const mem_region_t *r = region_at(d, pg);
                    uintptr_t f = r ? leaf_flags_for(r) : kflags;
                    if (vmm_map_range(root, pg, pg, PAGE_SIZE, f) != 0) return NULL;
                }
                break;
        }
    }
    return root;
}

int mem_domain_activate(const mem_domain_t *d) {
    /* X2, plan/phase23_multicore_scheduling.md: which harts have actually
     * activated a restricted domain.
     *
     * The milestone asks whether per-task isolation still holds "regardless
     * of which hart activates a given domain". That question cannot be
     * answered by a test that merely passes -- with driver tasks pinned to
     * hart 0, a run could satisfy every isolation check while never once
     * activating a domain anywhere else. This counter is what turns the
     * claim into evidence: a non-zero entry for hart 1 means a restricted
     * domain was installed there, by code running there. */
    if (d) g_domain_activations[hart_id()]++;
    if (!vmm_paging_enabled()) return 0; /* nothing to enforce with */

    if (!d) {
        /* Kernel task: the kernel's own space, which grants U nothing. */
        vmm_switch_space(NULL);
        return 0;
    }

    mem_domain_t *m = (mem_domain_t *)d; /* the cache below is the only write */
    if (!m->arch_priv) {
        m->arch_priv = build_space(d);
        if (!m->arch_priv) {
            printk("[MemDomain] Could not build an address space for this domain\n");
            return -1;
        }
    }

    vmm_space_t space = { .page_table_root = (uintptr_t *)m->arch_priv };
    vmm_switch_space(&space);
    return 0;
}

bool mem_domain_enforced(void) { return vmm_paging_enabled(); }

const char *mem_domain_backend_name(void) { return "Sv39"; }

/* Returns the page table built by build_space(). See the header on why this
 * must not run while the domain is active -- the hart would be translating
 * through the tree being freed. */
void mem_domain_destroy(mem_domain_t *d) {
    if (!d) return;
    if (d->arch_priv) {
        vmm_free_table((uintptr_t *)d->arch_priv);
        d->arch_priv = NULL;
    }
    d->count = 0;
}

#endif
