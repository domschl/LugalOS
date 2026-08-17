#ifndef LUGALOS_KERNEL_MEM_DOMAIN_H
#define LUGALOS_KERNEL_MEM_DOMAIN_H

#include <stdint.h>
#include <stdbool.h>

/* Per-task memory domains (B3, plan/phase5_distributed_design.md §5.4).
 *
 * A domain is the set of address ranges a task may touch, and the mechanism
 * that enforces it is the one thing that differs between builds: PMP regions
 * on the M-mode targets (B3), Sv39 page tables on the MMU target (B5). Rule 0
 * (§5.1) says the *interface* must not differ, so `task_t` carries a
 * `mem_domain *` on both and the scheduler activates it identically.
 *
 * ## The region budget is per-task, not per-system
 *
 * An earlier note in this plan read the measured "8 free PMP entries" as
 * bounding the number of isolatable tasks at 7. **That was wrong**, and the
 * correction matters for how B4 is designed.
 *
 * PMP describes the *currently running* hart's view. The domain is
 * reprogrammed on every context switch, so the eight entries bound how
 * complicated one task's view can be -- not how many tasks can exist. Task
 * count is bounded by memory, which on RP2350 means the 18-page heap, not by
 * the PMP table. Statically partitioning the entries across tasks would have
 * imposed that 7-task ceiling for no benefit.
 *
 * The cost is ~2 CSR writes per region on each switch, which is nothing
 * beside the work a switch already does.
 *
 * ## Regions are NAPOT, and RP2350 reverses the permission bits
 *
 * Hazard3 implements only OFF and NAPOT (datasheet §3.8.3.1), so regions are
 * power-of-two sized and self-aligned. RP2350 additionally reverses the R/W/X
 * bit order under erratum E6 -- see arch/riscv/common/mem_domain.c, where
 * that is handled and where the symptom it produced is recorded.
 *
 * Callers supply power-of-two, self-aligned base and size of at least 32
 * bytes (RP2350's granule; the stricter of the two targets' floors).
 * mem_domain_add() rejects anything else rather than silently rounding: a
 * region quietly widened to satisfy alignment is a hole in the isolation it
 * was asked to provide.
 */

#define MEM_R (1u << 0)
#define MEM_W (1u << 1)
#define MEM_X (1u << 2)

/* Five, which is exactly RP2350's dynamic budget: eight PMP entries less the
 * three that shadow Hazard3's hardwired U-mode grants (see mem_domain.c). The
 * QEMU targets have all eight, so this is the tighter of the two and the one
 * worth sizing against.
 *
 * Four sufficed while a user image was two fixed pages: stack, text, data. C4
 * lets an image span more, and a segment whose page count is not a power of
 * two has to be granted as several NAPOT pieces -- so the budget is now what
 * decides how large and how irregular an image may be, rather than an
 * arbitrary headroom figure. */
#define MEM_DOMAIN_MAX_REGIONS 5

typedef struct {
    uintptr_t base;
    uintptr_t size;   /* power of two, >= the hardware granularity */
    uint8_t   perms;
} mem_region_t;

typedef struct mem_domain {
    mem_region_t regions[MEM_DOMAIN_MAX_REGIONS];
    int          count;
    /* Backend-private. Unused by the PMP backend, which programs registers
     * directly; the Sv39 backend caches this domain's page table here,
     * because building one per context switch would be absurd where the PMP
     * path is a handful of CSR writes. Built lazily on first activation so
     * the interface stays the one B3 defined. */
    void        *arch_priv;
} mem_domain_t;

void mem_domain_init(mem_domain_t *d);

/* Returns 0, or -1 if the domain is full or the range is not a
 * power-of-two-sized, self-aligned block of at least 16 bytes. Ordering
 * matters: PMP resolves an address against the lowest-numbered matching
 * region, so a narrow grant must be added before a broader one that overlaps
 * it. */
int mem_domain_add(mem_domain_t *d, uintptr_t base, uintptr_t size, uint8_t perms);

/* Installs `d` as the active restriction set, or removes all restriction when
 * `d` is NULL (which is what running kernel-only tasks wants). Called by the
 * scheduler on every switch.
 *
 * Returns 0 if every region was installed exactly as asked, -1 if the
 * hardware altered one. A caller about to drop a task into U-mode must treat
 * -1 as fatal: a region the hardware rewrote is not a tighter restriction,
 * it is an unknown one, and running under it would claim isolation that has
 * not been verified. */
int mem_domain_activate(const mem_domain_t *d);

/* True if `base..base+len` lies entirely within one region of `d` that grants
 * every bit of `perms`. A NULL domain (an unrestricted kernel task) permits
 * everything.
 *
 * Used by the syscall boundary to validate user-supplied pointers. Reads the
 * region list rather than the hardware, so it is identical on both builds --
 * which means copy-in/copy-out is exercised by the whole suite, not only
 * where PMP happens to be enforcing. */
bool mem_domain_permits(const mem_domain_t *d, uintptr_t base, uintptr_t len,
                        uint8_t perms);

/* True when this build can actually enforce a domain. False on the S-mode
 * target until B5 lands Sv39 -- so a caller can report "unenforced" honestly
 * rather than implying isolation that is not there. */
/* Releases whatever the backend cached for this domain and leaves it empty
 * (C2, plan/phase6_memory_and_processes.md).
 *
 * The Sv39 backend builds a page table on first activation and caches it in
 * arch_priv; nothing could return it, which is why the loader had a single
 * reusable slot instead of a domain per program -- one per exec would have
 * leaked a tree every time. The PMP backend has nothing to release, so this
 * is where the two models stop differing again.
 *
 * Must not be called on a domain that is currently active: on the MMU build
 * that would free the page table the hart is translating through. Callers
 * destroy a program's domain only once its task is dead. */
void mem_domain_destroy(mem_domain_t *d);

bool mem_domain_enforced(void);

/* M6: "PMP" or "Sv39" -- which backend this build's domains are enforced by,
 * so a reporting surface (`ps`'s own "Isol" column) can say which kind of
 * hardware-backed isolation a task actually has, not just that it has one.
 * A compile-time fact (CONFIG_MODE_M vs CONFIG_MODE_S), so this is one
 * function rather than every caller re-deriving it from the same macros. */
const char *mem_domain_backend_name(void);

#endif /* LUGALOS_KERNEL_MEM_DOMAIN_H */
