#ifndef LUGALOS_KERNEL_LOCK_H
#define LUGALOS_KERNEL_LOCK_H

#include <stdint.h>
#include <stdbool.h>

/* Cross-hart locks (S1, plan/phase22_smp_locking_foundation.md).
 *
 * ## What this replaces, and why it is not a style change
 *
 * Every "no locking needed" note in this kernel rests on the premise stated
 * verbatim in kernel/include/kernel/irq.h: "the regions are a handful of
 * instructions, there is one hart, and a task that blocks while holding a
 * scheduler lock could not be scheduled out of it."
 *
 * `irq_save()`/`irq_restore()` disables interrupts on **the hart that calls
 * it** and nothing else. Against preemption and interrupt handlers on that
 * one hart it is real mutual exclusion. Against a second hart reading and
 * writing the same memory at the same instant it does exactly nothing,
 * because masking hart 0's interrupts has no effect on hart 1.
 *
 * ## Two types, deliberately
 *
 * `fs/p9_link.c` already built one of these by hand, under real pressure:
 * preemption turned an intermittent one-run-in-three failure in the two-node
 * 9P tests into a bug report, and the fix was a re-entrant lock that *yields*
 * rather than spins. That shape is correct and it is not the same shape a
 * page-allocator bitmap wants. Collapsing them into one API would just move
 * the mistake to whichever call site picks wrong, so:
 *
 *   spinlock_t  short, non-blocking critical sections -- a handful of
 *               instructions that never yield, wait or call anything that
 *               might. Busy-spins on contention, and holds interrupts off
 *               locally for the duration.
 *
 *   ylock_t     sections that can be held across a wait (a 9P round trip
 *               waits for a peer's reply). Re-entrant for the owning task,
 *               and yields rather than spinning.
 *
 * Choosing between them is a correctness decision, not a preference. A
 * `spinlock_t` taken where a `ylock_t` was needed deadlocks *on one hart,
 * today*: a task spinning on a lock that only the descheduled holder can
 * release never lets that holder run again. That is why the yielding variant
 * exists and why this header says so twice.
 *
 * ## The atomic
 *
 * Both types gate on arch_lock_try_acquire()/arch_lock_release()
 * (arch/riscv/include/arch/atomic.h), which is the only hart-count-sensitive
 * part and the only thing that would change if RP2350's Hazard3 turns out to
 * need its SIO hardware spinlocks instead of the A extension. On one hart
 * these behave exactly as the plain flag they replace, which is what makes it
 * safe to land them before a second core exists -- the whole existing suite
 * becomes a regression check on their placement.
 */

/* --- spinlock_t ------------------------------------------------------- */

typedef struct {
    volatile uint32_t word;   /* 0 free, 1 held */
} spinlock_t;

#define SPINLOCK_INIT { 0 }

void spinlock_init(spinlock_t *l);

/* Takes `l` with interrupts disabled locally, and returns the flags to hand
 * back to spin_unlock_irqrestore().
 *
 * Interrupts go off *before* the lock is taken, not after: taking it first
 * leaves a window where this hart holds the lock and can still be preempted
 * inside the critical section, which on one hart is precisely the bug
 * irq_save() was introduced to close and would remain a bug on two.
 *
 * Never call this while already holding `l`. spinlock_t is NOT re-entrant --
 * a second acquire from the same hart spins forever on a lock only that hart
 * could release. Use ylock_t where re-entry is possible. */
uintptr_t spin_lock_irqsave(spinlock_t *l);
void spin_unlock_irqrestore(spinlock_t *l, uintptr_t flags);

/* True if held by anyone. For assertions and diagnostics only: by the time a
 * caller acts on the answer it may be stale, which is exactly why this is not
 * a lock-acquisition primitive. */
bool spin_is_locked(const spinlock_t *l);

/* --- ylock_t ---------------------------------------------------------- */

typedef struct {
    volatile uint32_t word;   /* 0 free, 1 held -- the cross-hart gate */
    volatile int      owner;  /* holding task's pid; only meaningful at depth>0 */
    volatile int      depth;  /* re-entry count; 0 when free */
} ylock_t;

/* All-zero is a valid, free ylock_t, so a static one needs no initializer.
 *
 * `depth` is the sole authority on whether the lock is held; `owner` is only
 * read when depth > 0, and ylock_owner() reports -1 below that regardless of
 * what the field holds. So the zero state and an explicitly-initialised one
 * are indistinguishable through the API.
 *
 * That is worth spelling out because the tempting alternative -- a
 * `{ 0, -1, 0 }` initialiser, which reads better -- has a non-obvious cost on
 * the target where memory is tightest. An initialised static lands in .data,
 * and on RP2350 .data sits inside an executable PT_LOAD (linker/rp2350.ld's
 * .ramfunc), so `nm` types it `t` and tools/sizereport.py -- which counts
 * only `b` and `d` -- does not see it at all. Writing S2's two locks that way
 * made the RP2350 budget report a 24-byte *saving* for memory that had merely
 * become invisible. Left zero, they stay in .bss and stay counted. See
 * plan/open_issues.md. */
void ylock_init(ylock_t *l);

/* Takes `l`, yielding the CPU while another task holds it.
 *
 * Re-entrant **for the owning task**: a task already holding `l` increments a
 * depth counter and returns immediately. That is not a convenience -- a
 * locally-mounted 9P namespace can be walked into recursively (`/self/self/`),
 * which re-enters the client path on the same task, and a plain lock would
 * deadlock there against itself.
 *
 * Safe to hold across a blocking wait, which is the other half of why it
 * yields: masking interrupts across a wait would stop the very timer that
 * lets the awaited event be processed. */
void ylock_acquire(ylock_t *l);
void ylock_release(ylock_t *l);

/* The pid holding `l`, or -1 when it is free -- which is decided by depth,
 * not by the owner field, so an all-zero lock reports -1 rather than pid 0.
 * Diagnostics and selftests; see spin_is_locked() on why a caller must not
 * gate an acquisition on it. */
int  ylock_owner(const ylock_t *l);
int  ylock_depth(const ylock_t *l);

/* Prints the checks and a LOCK_SELFTEST_OK / _FAIL marker; returns the number
 * of failures. */
int lock_selftest(void);

#endif /* LUGALOS_KERNEL_LOCK_H */
