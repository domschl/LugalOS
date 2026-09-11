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
/* Declared as macros further down, so the call site's own text becomes the
 * lock's name in a diagnostic. */

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
/* ylock_acquire() is a macro further down, for the same reason. */
void ylock_release(ylock_t *l);

/* The pid holding `l`, or -1 when it is free -- which is decided by depth,
 * not by the owner field, so an all-zero lock reports -1 rather than pid 0.
 * Diagnostics and selftests; see spin_is_locked() on why a caller must not
 * gate an acquisition on it. */
int  ylock_owner(const ylock_t *l);
int  ylock_depth(const ylock_t *l);

/* --- The hierarchy, and the check that keeps it honest (Y2,
 * plan/phase31_concurrency_hierarchy.md) -----------------------------------
 *
 * ## The rule, in one line
 *
 * **A `spinlock_t` is a leaf: while one is held, nothing may be acquired and
 * nothing may block.**
 *
 * That is stronger than the graduated level system phase 31 first proposed,
 * and it is what the tree already does. Y0 walked every critical section in
 * `kernel/`, `drivers/`, `fs/` and `net/`: each one calls only pure helpers --
 * `bit_get`/`bit_set`, `hart_id`, `memcpy`, `node_parent` -- and where a
 * section appeared to call something blocking it turned out to release and
 * re-take around it, with a comment saying why (`uart_putc()` around
 * `uart_flush()`, `printk_lock()` around `task_block()`). The only two
 * exceptions were bugs, both in `task_exit()`, and both are fixed.
 *
 * A leaf rule needs one comparison, where levels need a table that has to be
 * kept right. Simpler, and it happens to be true.
 *
 * ## `ylock_t` is deliberately not covered by it
 *
 * A ylock exists *to* be held across a wait -- see its own comment above --
 * so "no blocking while held" would forbid the thing it is for.
 * `fs/p9_link.c` holds `g_pump_lock` across a whole 9P transaction and
 * `net/mqtt.c` holds `g_client_lock` across `write_all()`, both by design.
 * What a ylock may not do is sit *under* a spinlock, because then the
 * spinlock is held across the ylock's yield -- so `ylock_acquire()` is
 * checked, and holding a ylock is not.
 *
 * ## Why per-hart state is the right shape, and survives the hand-off
 *
 * `g_sched_lock` is acquired by `task_exit()` and released by a *different
 * task* -- the successor inherits it. Its critical section is therefore not
 * lexical, which defeats any checker that pairs an acquire with a release in
 * the same function. Per-hart counting does not care: a hand-off happens
 * across `ctx_switch()`, which stays on one hart, so the increment and the
 * decrement land on the same counter regardless of which task performs them.
 *
 * That is also why the state cannot be per-task. A task may migrate harts,
 * but only across a block -- and a spinlock is never held across one.
 *
 * ## What a violation does
 *
 * Reports and continues, like `handoff_check()`'s fault counter and unlike
 * `sched_check_free_range()`'s halt. The report is emitted *before* the
 * acquisition that would hang, so the failure phase 31 exists to prevent --
 * a board that stops dead with no clue -- becomes a named diagnostic followed
 * by the same hang. Halting instead would turn a latent ordering bug into a
 * dead board on the strength of a check that has not yet earned that trust.
 */

/* The spinlock this hart is holding, or NULL. Diagnostics only. */
const char *lock_spin_held(void);
const char *lock_spin_site(void);

/* Violations counted since boot, and the last one described. `lockcheck` and
 * the selftest read these; nothing changes behaviour on them. */
uint32_t    lock_faults(void);

/* Reports if this hart holds a spinlock, naming it and the site. For the
 * blocking primitives -- task_block(), chan_call(), ylock_acquire() -- which
 * must not be reached with one held. Returns true if a violation was found,
 * so a caller that can refuse may. */
bool lock_check_may_block(const char *what);
bool lock_check_may_block_named(const char *what, const char *name,
                                const char *site);

/* The real entry points. The macros below capture the lock's name and the
 * calling function at the call site, which costs .rodata (flash on RP2350)
 * rather than a field in every spinlock_t. */
uintptr_t spin_lock_irqsave_at(spinlock_t *l, const char *name, const char *site);
void      spin_unlock_irqrestore_at(spinlock_t *l, uintptr_t flags);
void      ylock_acquire_at(ylock_t *l, const char *name, const char *site);

#define spin_lock_irqsave(l)             spin_lock_irqsave_at((l), #l, __func__)
#define spin_unlock_irqrestore(l, f)     spin_unlock_irqrestore_at((l), (f))
#define ylock_acquire(l)                 ylock_acquire_at((l), #l, __func__)

/* Prints the checks and a LOCK_SELFTEST_OK / _FAIL marker; returns the number
 * of failures. */
int lock_selftest(void);

#endif /* LUGALOS_KERNEL_LOCK_H */
