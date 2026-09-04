#ifndef LUGALOS_KERNEL_HART_H
#define LUGALOS_KERNEL_HART_H

/* Per-hart state, and the one register that points at it (S0,
 * plan/phase22_smp_locking_foundation.md §6.2).
 *
 * ## The problem this exists to solve
 *
 * Before this, a hart could not find out which hart it was. That sounds like
 * a one-line `csrr mhartid`, and on the M-mode targets it very nearly is --
 * but `mhartid` is a *machine*-mode CSR, and arch/riscv/common/entry.S
 * performs the M->S transition itself on the RV64 build (there is no SBI
 * firmware underneath to ask). Reading it from S-mode traps as an illegal
 * instruction. So the value has to be captured once, at reset, while the
 * hart is still in M-mode, and kept somewhere S-mode can reach.
 *
 * Both of the conventional places to keep it were already occupied:
 *
 *   - `tp` is the RISC-V thread pointer and the usual home for exactly this,
 *     but entry.S saves and restores it as an ordinary task register in the
 *     trap frame, so it round-trips through user code.
 *   - `CSR_SCRATCH` (mscratch/sscratch) held the current task's kernel stack
 *     pointer while U-mode ran, and *zero* while the kernel ran, so that one
 *     `csrrw` both switched stacks and revealed which side the trap came
 *     from. That invariant is load-bearing: it is why no separate "am I in
 *     user mode" flag exists to fall out of sync with reality.
 *
 * ## What changed
 *
 * The scratch CSR now holds a pointer to *this record* while U-mode runs,
 * instead of the bare kernel stack pointer -- and the kernel stack pointer
 * moved into the record's first word. The discrimination in the trap vector
 * is untouched in shape and strictly safer in substance: it still branches on
 * "did a non-zero value come back", but the non-zero value is now a .bss
 * address fixed at link time rather than a stack pointer that a sufficiently
 * broken task could in principle have zeroed.
 *
 * `tp` then holds this record's address for the whole time the kernel is
 * executing, which is what makes hart_self() a register read rather than a
 * CSR access, and what lets it work identically in M-mode and S-mode. It is
 * still saved and restored across U-mode, because user code owns its own tp;
 * the trap vector writes the *user's* tp into the frame and installs the
 * kernel's in the same breath (see arch/riscv/common/entry.S).
 *
 * ## Scope
 *
 * MAX_HARTS is 2 because RP2350 has exactly two cores. Nothing here wakes a
 * second one -- that is phase 23 -- and on every target today g_harts[1] is
 * simply never touched. The point of landing this first is that it is
 * behaviour-preserving on one hart by construction, in the same way the
 * original irq_save()/irq_restore() work was landed before preemption could
 * exercise it: the whole existing suite becomes a regression check on the
 * trap-path restructure before anything can actually contend.
 */

#define MAX_HARTS 2

/* The value the primary hart writes to g_smp_release once the kernel is far
 * enough along for a secondary to run C code (X1,
 * plan/phase23_multicore_scheduling.md).
 *
 * A magic value rather than a plain flag, because a secondary spins on this
 * word while the primary is still *clearing .bss*, where the word lives.
 * Before that clear it holds whatever the RAM held, so "nonzero means go"
 * could release a hart into a half-initialised kernel. Requiring one
 * specific 32-bit value makes an accidental release vanishingly unlikely --
 * the same reasoning behind STACK_POISON's improbable byte-uniform pattern
 * (kernel/include/kernel/meminfo.h). */
#define SMP_RELEASE_MAGIC 0x5AFEC0DE

#if __riscv_xlen == 64
#define HART_REGBYTES   8
#define HART_SIZE_SHIFT 5
#else
#define HART_REGBYTES   4
#define HART_SIZE_SHIFT 4
#endif

/* Byte offsets into hart_t, shared with the assembly below and with
 * arch/riscv/common/{entry,umode}.S. kernel/hart.c static-asserts these
 * against the C struct, so the two cannot drift silently. */
#define HART_OFF_KERNEL_SP (0 * HART_REGBYTES)
#define HART_OFF_ID        (1 * HART_REGBYTES)
#define HART_OFF_TMP       (2 * HART_REGBYTES)
#define HART_SIZE          (1 << HART_SIZE_SHIFT)

#ifdef __ASSEMBLER__

/* Turns the raw hart id parked in `tp` at reset into this hart's record
 * pointer, and leaves the scratch CSR at its kernel-side value.
 *
 * A macro, and living here next to the offsets, for the same reason
 * PAINT_BOOT_STACK does (kernel/include/kernel/meminfo.h): **this kernel has
 * two boot paths and they do not share one**. The QEMU targets enter at
 * `_start` (arch/riscv/common/entry.S); RP2350 enters at `_reset_handler`
 * (arch/riscv/rp2350/boot_header.S) and never executes `_start` at all.
 * Establishing the hart pointer in one of them would leave the other running
 * with a garbage `tp` -- and since the trap vector now dereferences it, that
 * is not a degraded diagnostic the way an unpainted stack was. It is a wild
 * store on the first trap.
 *
 * Invoke with `tp` still holding the raw id and both temporaries dead. Must
 * run *after* .bss is cleared -- g_harts lives there, and a clear that ran
 * afterwards would erase the id this just wrote.
 */
.macro SETUP_HART_POINTER ta, tb
    mv \ta, tp
    la tp, g_harts
    slli \tb, \ta, HART_SIZE_SHIFT
    add tp, tp, \tb
#if __riscv_xlen == 64
    sd \ta, HART_OFF_ID(tp)
    sd zero, HART_OFF_KERNEL_SP(tp)
#else
    sw \ta, HART_OFF_ID(tp)
    sw zero, HART_OFF_KERNEL_SP(tp)
#endif
    /* The scratch CSR's reset value is architecturally undefined, and the
     * trap vector reads it before anything has written it -- on RP2350 the
     * bootrom has been running in this hart's CSRs for a while by now. A
     * garbage value there would send the very first kernel trap down the
     * from-user path and dereference it. Nothing zeroed it before this
     * commit; the kernel simply got away with whatever reset left behind. */
#if defined(CONFIG_MODE_S)
    csrw sscratch, zero
#else
    csrw mscratch, zero
#endif
.endm

#else /* !__ASSEMBLER__ */

#include <stdint.h>

typedef struct hart {
    /* The current task's kernel stack pointer, parked here while that task
     * runs in U-mode so the trap vector can switch back onto it. Meaningful
     * only while the scratch CSR points at this record, i.e. only while
     * U-mode is running; stale at every other moment, deliberately, because
     * maintaining it in the kernel would be bookkeeping nothing reads. */
    uintptr_t kernel_sp;
    /* mhartid, captured at reset while still in M-mode. */
    uintptr_t id;
    /* One word the trap vector can spill a register into on the way in from
     * U-mode, before it has a stack to spill onto. Safe as a single slot
     * because a trap from U-mode cannot nest: the hardware has already
     * masked interrupts by the time the vector runs, and a fault taken
     * inside the eight instructions that use this is unrecoverable anyway. */
    uintptr_t tmp;
    uintptr_t _reserved;
} hart_t;

extern hart_t g_harts[MAX_HARTS];

/* Set to SMP_RELEASE_MAGIC by the primary hart once secondaries may run.
 * Read by arch/riscv/common/entry.S's .Lsecondary_wait loop. */
extern volatile uint32_t g_smp_release;

/* Wakes the secondary harts. Called by the primary once the scheduler is up.
 * A no-op unless CONFIG_ENABLE_SMP. */
void smp_release_secondaries(void);

/* Entry point for a secondary hart, called from entry.S with this hart's
 * record already established. Does not return. */
void secondary_main(void);

/* How many harts have entered the scheduler, primary included. */
unsigned smp_harts_online(void);

/* X1's concurrency check: workers incrementing a shared counter through a
 * lock, asserting no lost updates AND that the work ran on more than one
 * hart. Prints SMP_SELFTEST_OK/_FAIL; returns the failure count. */
int smp_selftest(void);

/* Launches RP2350's core 1 over the SIO FIFO (X3). Explicit rather than
 * automatic at boot: see the definition. Returns 0 on success. */
int smp_start_secondary(void);

/* This hart's record. Valid from the moment SETUP_HART_POINTER runs, which
 * is before kernel_main() on both boot paths, so every caller in C is safe
 * by construction. */
static inline hart_t *hart_self(void) {
    hart_t *h;
    __asm__ ("mv %0, tp" : "=r"(h));
    return h;
}

/* Which hart is executing this. Zero on every target today. */
static inline unsigned hart_id(void) {
    return (unsigned)hart_self()->id;
}

#endif /* __ASSEMBLER__ */

#endif /* LUGALOS_KERNEL_HART_H */
