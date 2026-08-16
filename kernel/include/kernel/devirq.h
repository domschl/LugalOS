#ifndef LUGALOS_KERNEL_DEVIRQ_H
#define LUGALOS_KERNEL_DEVIRQ_H

#include <stdint.h>

/* Device interrupt dispatch (M2, plan/phase12_microkernel_migration.md).
 *
 * Every blocking wait in this tree, before M2, was a `while (!ready)
 * sched_yield();` spin -- `task_block()`/`task_unblock()` (B2) had zero
 * call sites outside kernel/sched.c itself. This is the missing half: a
 * place for a peripheral's own interrupt handler to call task_unblock() on
 * whoever is waiting for it, so a driver can actually sleep instead of
 * polling every scheduler tick.
 *
 * Arch-independent by construction, per Rule 0 (§5.1,
 * plan/phase5_distributed_design.md): this table only knows "IRQ number ->
 * handler". *Identifying* which IRQ number fired is a different problem on
 * every target -- QEMU's PLIC claim/complete register vs. RP2350's Hazard3
 * Xh3irq `meinext` CSR share no mechanism at all -- so that part stays in
 * arch/riscv/common/trap.c's per-target branches, which call
 * devirq_dispatch() once they've resolved a number. Everything above that
 * line (every driver's ISR) is identical on both builds.
 */

typedef void (*devirq_handler_fn)(void *ctx);

/* One handler per IRQ number. Refused, not replaced, if `irq_num` already
 * has one -- two ISRs both believing they own the same line is exactly the
 * bug this table exists to make impossible, matching chan_register()'s
 * "duplicate name refused" precedent for the same reason. */
int devirq_attach(uint32_t irq_num, devirq_handler_fn handler, void *ctx);

/* Called only from arch/riscv/common/trap.c, once it has identified which
 * IRQ number fired. A lookup miss is reported once via printk and otherwise
 * ignored -- the same "unhandled interrupt" outcome this replaces, not a
 * new failure mode. */
void devirq_dispatch(uint32_t irq_num);

#endif /* LUGALOS_KERNEL_DEVIRQ_H */
