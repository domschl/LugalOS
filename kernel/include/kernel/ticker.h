#ifndef LUGALOS_KERNEL_TICKER_H
#define LUGALOS_KERNEL_TICKER_H

#include <stdint.h>
#include <stdbool.h>

/* Periodic timer interrupt for preemption (B6, plan §5.4).
 *
 * The timer is the one piece of B6 that is genuinely per-target, and not for
 * stylistic reasons -- the three builds have three different clocks reached
 * three different ways:
 *
 *   RP2350 (M-mode)   SIO mtime/mtimecmp, a RISC-V platform timer in the
 *                     core-local peripheral block. Needs enabling.
 *   QEMU RV32 (M-mode) CLINT mtimecmp, memory-mapped at 0x2000000.
 *   QEMU RV64 (S-mode) neither of those is reachable: CLINT registers are
 *                     M-mode, and this build enters S-mode from entry.S with
 *                     no SBI firmware underneath to call. It uses the Sstc
 *                     extension's stimecmp CSR instead, which M-mode has to
 *                     unlock by setting menvcfg.STCE before dropping down.
 *
 * So this header is the seam: one interface, three small backends. Everything
 * above it -- the scheduler, the preemption policy -- is identical, which is
 * Rule 0 applied where it belongs rather than pretending the hardware is the
 * same when it is not.
 */

/* Programs the first deadline and unmasks the timer interrupt. Returns false
 * if this target has no usable timer, so preemption can stay off and say so
 * rather than silently never firing. */
bool ticker_init(uint32_t hz);

/* Acknowledges the current tick by programming the next deadline. On RISC-V a
 * timer interrupt is level-triggered off mtime >= mtimecmp, so it stays
 * pending until the comparator moves -- forgetting this does not drop a tick,
 * it hangs the machine in a trap loop. */
void ticker_next(void);

bool     ticker_enabled(void);
uint64_t ticker_ticks(void);
void     ticker_count_tick(void);

#endif /* LUGALOS_KERNEL_TICKER_H */
