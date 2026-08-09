#include "arch/umode.h"
#include "arch/csr.h"
#include "kernel/printk.h"

/* U-mode support (B3, plan/phase5_distributed_design.md §5.4).
 *
 * ## Why a PMP grant is needed *before* the first U-mode instruction
 *
 * On the M-mode targets (RV32 QEMU, RP2350) PMP defaults to deny for U-mode:
 * if no entry matches an address, M-mode may access it and U-mode may not. So
 * dropping to U-mode with no entry configured does not produce a task with
 * limited access -- it produces one that cannot fetch its own first
 * instruction. RP2350's preconfigured entries 8/9/10 cover the boot ROM,
 * system peripherals and SIO, none of which contain this kernel's code.
 *
 * That is why the mode transition and PMP cannot be brought up strictly one
 * after the other on these targets: U-mode is unreachable until at least one
 * region grants it something.
 *
 * ## What this stage does and does not claim
 *
 * arch_user_grant_all() installs a single entry covering the whole address
 * space, RWX. **That is not isolation** -- it is the minimum that makes the
 * transition observable, so the mode switch and the trap path's stack swap can
 * be proven before per-task regions are layered on. Real per-task regions are
 * the next step; until then a U-mode task is separated by privilege level
 * (it cannot execute privileged instructions or touch CSRs) but not by
 * address range.
 *
 * The S-mode target needs none of this: PMP CSRs are M-mode only, entry.S
 * already opened pmp0 before dropping to S-mode, and with satp in Bare mode
 * U-mode addressing is unrestricted. Its enforcement mechanism is Sv39 in B5,
 * per D2.
 */

#if defined(CONFIG_MODE_M)

/* NAPOT encoding: an all-ones pmpaddr describes a region based at 0 whose
 * size is the whole address space. Writing all ones and reading back is also
 * how pmp_probe() measures granularity -- see arch/riscv/common/pmp_probe.c. */
#define PMP_A_NAPOT (3u << 3)
#define PMP_R       (1u << 0)
#define PMP_W       (1u << 1)
#define PMP_X       (1u << 2)

static uintptr_t entry0_cfg_mask(void) {
    return (uintptr_t)0xff; /* entry 0 occupies the low config byte */
}

void arch_user_grant_all(void) {
    /* Entry 0 deliberately: PMP matches the lowest-numbered entry first, so
     * this takes precedence over RP2350's preconfigured 8/9/10 without
     * disturbing them. They stay available for whatever B3's real region
     * layout decides to do with U-mode's peripheral access. */
    write_csr(pmpaddr0, (uintptr_t)-1);

    uintptr_t cfg = read_csr(pmpcfg0);
    cfg &= ~entry0_cfg_mask();
    cfg |= (PMP_A_NAPOT | PMP_R | PMP_W | PMP_X);
    write_csr(pmpcfg0, cfg);
}

void arch_user_revoke_all(void) {
    /* Clearing A (address-match mode) deactivates the entry; the address
     * register is left as-is since an inactive entry never matches.
     *
     * This exists because the grant is *not* boot state and must not outlive
     * the U-mode task that needed it. Found by the hardware suite rather than
     * by reasoning: after `usertest` ran once, pmpinfo started reporting one
     * more active region and one fewer free, permanently -- a wide-open
     * U-mode grant left installed for the rest of the system's life. Per-task
     * regions will make this scoping structural; until then it is explicit. */
    uintptr_t cfg = read_csr(pmpcfg0);
    cfg &= ~entry0_cfg_mask();
    write_csr(pmpcfg0, cfg);
}

#else /* CONFIG_MODE_S */

/* Nothing to do, and nothing that *could* be done: PMP CSRs are M-mode only
 * and this build already runs in S-mode. Its enforcement mechanism is Sv39 in
 * B5, per D2. */
void arch_user_grant_all(void) {
}

void arch_user_revoke_all(void) {
}

#endif
