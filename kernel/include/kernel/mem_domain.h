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
 * ## Regions are NAPOT, and the order of the CSR writes matters
 *
 * Hazard3 does not implement TOR (writing A=TOR reads back as A=OFF), so
 * NAPOT is the only mode available. Its size-in-trailing-ones encoding is
 * fragile here: pmpaddr's low bits are masked at write time according to the
 * A mode then in effect, so the config register must be written before the
 * address or the encoding is silently corrupted. See
 * arch/riscv/common/mem_domain.c for both findings and how they presented.
 *
 * Callers therefore supply power-of-two, self-aligned base and size (at least
 * the 16-byte measured granularity).
 * mem_domain_add() rejects anything else rather than silently rounding: a
 * region quietly widened to satisfy alignment is a hole in the isolation it
 * was asked to provide.
 */

#define MEM_R (1u << 0)
#define MEM_W (1u << 1)
#define MEM_X (1u << 2)

/* Four is comfortably under the measured budget of eight free entries, and
 * more than a task needs (its stack, plus executable text). The headroom is
 * for B4's servers, which may want a device window. */
#define MEM_DOMAIN_MAX_REGIONS 4

typedef struct {
    uintptr_t base;
    uintptr_t size;   /* power of two, >= the hardware granularity */
    uint8_t   perms;
} mem_region_t;

typedef struct mem_domain {
    mem_region_t regions[MEM_DOMAIN_MAX_REGIONS];
    int          count;
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

/* True when this build can actually enforce a domain. False on the S-mode
 * target until B5 lands Sv39 -- so a caller can report "unenforced" honestly
 * rather than implying isolation that is not there. */
bool mem_domain_enforced(void);

#endif /* LUGALOS_KERNEL_MEM_DOMAIN_H */
