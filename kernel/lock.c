#include "kernel/lock.h"
#include "kernel/irq.h"
#include "kernel/sched.h"
#include "kernel/ticker.h"
#include "kernel/time.h"
#include "kernel/console.h"
#include "arch/atomic.h"
#include <stddef.h>

/* See kernel/include/kernel/lock.h for the rationale. */

void spinlock_init(spinlock_t *l) { if (l) l->word = 0; }

uintptr_t spin_lock_irqsave(spinlock_t *l) {
    uintptr_t flags = irq_save();
    while (!arch_lock_try_acquire(&l->word)) {
        /* Spin. Nothing else is correct here: the holder is either running on
         * another hart (so waiting is exactly right) or is a handful of
         * instructions from releasing on this one. Yielding instead would
         * make this a ylock_t, with a ylock_t's rules -- see the header. */
    }
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *l, uintptr_t flags) {
    arch_lock_release(&l->word);
    irq_restore(flags);
}

bool spin_is_locked(const spinlock_t *l) { return l && l->word != 0; }

void ylock_init(ylock_t *l) {
    if (!l) return;
    l->word = 0;
    l->owner = -1;
    l->depth = 0;
}

void ylock_acquire(ylock_t *l) {
    int me = sched_current_pid();
    for (;;) {
        uintptr_t f = irq_save();

        /* Re-entry by the owner, checked before the gate.
         *
         * `depth > 0` is tested as well as the owner, and not just for
         * tidiness: `owner` keeps the last holder's pid until the next
         * acquire overwrites it, so on a free lock it can still name us. Only
         * a non-zero depth means the lock is actually held. */
        if (l->depth > 0 && l->owner == me) {
            l->depth++;
            irq_restore(f);
            return;
        }

        if (arch_lock_try_acquire(&l->word)) {
            l->owner = me;
            l->depth = 1;
            irq_restore(f);
            return;
        }

        /* Held by someone else. Drop interrupts back and let them run --
         * outside the masked region, because sched_yield() switching away
         * with interrupts still off would carry that state into whatever
         * runs next. */
        irq_restore(f);
        sched_yield();
    }
}

void ylock_release(ylock_t *l) {
    uintptr_t f = irq_save();
    if (l->depth > 0 && --l->depth == 0) {
        l->owner = -1;
        /* Released last, after the bookkeeping: another hart that sees the
         * gate free must not then read an owner this hart has not cleared
         * yet. arch_lock_release()'s release ordering is what makes "after"
         * mean anything at all here. */
        arch_lock_release(&l->word);
    }
    irq_restore(f);
}

int ylock_owner(const ylock_t *l) {
    /* depth, not owner, decides "free" -- see the header: that is what makes
     * an all-zero static a valid unlocked lock rather than one owned by
     * pid 0. */
    return (l && l->depth > 0) ? l->owner : -1;
}
int ylock_depth(const ylock_t *l) { return l ? l->depth : 0; }

/* --- selftest --------------------------------------------------------- */

/* Three claims, and each one is checked against a *measured* effect rather
 * than against the implementation restating itself. That standard is
 * kernel/random.c's: a test that asserts "the flag we just set is set" passes
 * on hardware that does nothing.
 *
 * What can and cannot be proven here is worth being exact about. Every target
 * runs one hart, so nothing below observes real contention between two harts
 * -- that first happens at phase 23's X1. What these do prove is that the
 * primitives behave correctly under the concurrency this kernel actually has
 * today (preemption and yielding), which is the property S2-S6 are about to
 * build on. */

static int g_fail;

static void check(const char *what, bool ok) {
    cprintf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_fail++;
}

/* The contender for check 3. Runs as its own task, tries to take a lock the
 * selftest is holding, and records the order of events so the main task can
 * tell "it never ran" apart from "it ran and correctly did not get in". */
static ylock_t        g_test_ylock;
static volatile int   g_contender_ran;      /* times it was scheduled at all */
static volatile bool  g_contender_acquired; /* did it get past ylock_acquire */
static volatile bool  g_contender_done;

static void contender_task(void *arg) {
    (void)arg;
    g_contender_ran++;
    ylock_acquire(&g_test_ylock);
    g_contender_acquired = true;
    ylock_release(&g_test_ylock);
    g_contender_done = true;
}

/* Busy-waits `us` microseconds against the hardware timer.
 *
 * time_get_us() reads a free-running counter, not a tick the interrupt
 * handler maintains, so it keeps advancing while interrupts are masked --
 * which is the entire reason this can measure what it measures. Using a
 * tick-derived clock here would stop when the thing under test stops and the
 * wait would never end. */
static void busy_wait_us(uint64_t us) {
    uint64_t end = time_get_us() + us;
    while (time_get_us() < end) { }
}

int lock_selftest(void) {
    g_fail = 0;
    cprintf("Lock primitive selftest:\n");

    /* --- 1. spinlock_t: acquire, release, and acquire again ------------ */
    {
        spinlock_t l;
        spinlock_init(&l);
        bool free_at_rest = !spin_is_locked(&l);

        uintptr_t f = spin_lock_irqsave(&l);
        bool held_inside = spin_is_locked(&l);
        spin_unlock_irqrestore(&l, f);
        bool free_after = !spin_is_locked(&l);

        /* Taking it a second time is the check with teeth: a release that
         * left the word set would pass the two above and deadlock here, so
         * this is what proves arch_lock_release() actually released. */
        f = spin_lock_irqsave(&l);
        spin_unlock_irqrestore(&l, f);

        check("spinlock: free, held, free, and can be retaken",
              free_at_rest && held_inside && free_after);
    }

    /* --- 2. the acquire is genuinely atomic ---------------------------- */
    {
        /* arch_lock_try_acquire() must succeed exactly once between
         * releases -- its whole contract, and the thing every caller here
         * relies on.
         *
         * Be exact about what this can and cannot show. Executed serially on
         * one hart, a plain `if (!held) { held = true; }` produces the same
         * three answers, so this does NOT distinguish an atomic swap from
         * the non-atomic flag it replaces. Atomicity is only observable when
         * two harts race the same word, which no target here can do until
         * phase 23's X1. What this pins down is the contract and the
         * release: a gate that stayed set, or one that let a second acquire
         * through, fails here rather than at the first real contention. */
        spinlock_t l;
        spinlock_init(&l);
        bool first  = arch_lock_try_acquire(&l.word);
        bool second = arch_lock_try_acquire(&l.word);
        arch_lock_release(&l.word);
        bool third  = arch_lock_try_acquire(&l.word);
        arch_lock_release(&l.word);
        check("atomic gate: exactly one acquire succeeds per release",
              first && !second && third);
    }

    /* --- 3. an irqsave-held spinlock really masks interrupts ----------- */
    if (!ticker_enabled()) {
        cprintf("  [skip] interrupt masking: no preemption timer on this build\n");
    } else {
        /* The measurable effect, per random.c's standard: the preemption
         * tick counter. It is incremented by the timer interrupt handler and
         * by nothing else, so if masking works it cannot move while the lock
         * is held, and if masking is a no-op it certainly will over a window
         * this long. Both directions are checked -- "it did not move" alone
         * would also pass on a board whose timer never fires. */
        const uint64_t window_us = 40000;   /* comfortably several ticks */

        spinlock_t l;
        spinlock_init(&l);

        uint64_t before_held = ticker_ticks();
        uintptr_t f = spin_lock_irqsave(&l);
        busy_wait_us(window_us);
        uint64_t during_held = ticker_ticks();
        spin_unlock_irqrestore(&l, f);

        uint64_t before_free = ticker_ticks();
        busy_wait_us(window_us);
        uint64_t after_free = ticker_ticks();

        bool masked   = (during_held == before_held);
        bool unmasked = (after_free > before_free);
        if (!masked || !unmasked) {
            cprintf("      ticks held: %lu -> %lu, free: %lu -> %lu over %lu us each\n",
                    (unsigned long)before_held, (unsigned long)during_held,
                    (unsigned long)before_free, (unsigned long)after_free,
                    (unsigned long)window_us);
        }
        check("spinlock: interrupts really are masked across the hold "
              "(tick counter frozen while held, moving while free)",
              masked && unmasked);
    }

    /* --- 4. ylock_t re-entry, and that a contender yields -------------- */
    {
        ylock_init(&g_test_ylock);
        g_contender_ran = 0;
        g_contender_acquired = false;
        g_contender_done = false;

        int me = sched_current_pid();
        ylock_acquire(&g_test_ylock);
        bool owned_once = (ylock_owner(&g_test_ylock) == me) &&
                          (ylock_depth(&g_test_ylock) == 1);

        /* Re-entry by the owner. A plain lock deadlocks on this line; that is
         * the whole reason ylock_t counts depth. */
        ylock_acquire(&g_test_ylock);
        bool reentered = (ylock_depth(&g_test_ylock) == 2);

        int pid = task_create("locktest", contender_task, NULL);
        bool spawned = (pid >= 0);

        /* Let the contender run repeatedly. It must be *scheduled* -- proving
         * it really is yielding rather than never getting a turn -- and must
         * still not be inside the lock. */
        for (int i = 0; i < 20 && spawned; i++) sched_yield();
        bool contender_scheduled = (g_contender_ran > 0);
        bool kept_out = !g_contender_acquired;

        /* One release is not enough: depth is 2. If the outer release handed
         * the lock over anyway, the contender would get in here, which is
         * what makes this a test of the depth counter rather than of the
         * gate. */
        ylock_release(&g_test_ylock);
        bool still_held = (ylock_depth(&g_test_ylock) == 1);
        for (int i = 0; i < 10 && spawned; i++) sched_yield();
        bool kept_out_at_depth_1 = !g_contender_acquired;

        ylock_release(&g_test_ylock);
        bool now_free = (ylock_depth(&g_test_ylock) == 0) &&
                        (ylock_owner(&g_test_ylock) == -1);

        for (int i = 0; i < 200 && spawned && !g_contender_done; i++) sched_yield();
        bool contender_got_in = g_contender_acquired && g_contender_done;

        check("ylock: the owner re-enters, depth counts up",
              owned_once && reentered);
        check("ylock: a different task is scheduled but stays out while held",
              spawned && contender_scheduled && kept_out && still_held && kept_out_at_depth_1);
        check("ylock: the last release frees it and the waiter proceeds",
              spawned && now_free && contender_got_in);
    }

    /* --- 5. the scheduler lock is handed across every switch (S6) ------ */
    {
        /* Everything above already drove the scheduler hard -- the contender
         * task was created, scheduled, blocked on a lock, resumed and
         * exited, and this task yielded through all of it. So by the time we
         * get here every landing site has been exercised: sched_yield()'s
         * return path, task_start()'s first run, and task_exit()'s hand to a
         * successor.
         *
         * A few more yields for good measure, then the count. Zero is the
         * only acceptable answer: a single fault means some path released
         * the lock before ctx_switch() rather than after, which on one hart
         * merely trips this counter and on two would be the stale-sp stack
         * corruption the whole rule exists to prevent. */
        for (int i = 0; i < 20; i++) sched_yield();
        uint32_t faults = sched_handoff_faults();
        if (faults) cprintf("      %u resume(s) arrived without the lock\n",
                            (unsigned)faults);
        check("scheduler lock is handed across ctx_switch, not dropped before it",
              faults == 0);
    }

    if (g_fail == 0) cprintf("LOCK_SELFTEST_OK (7/7)\n");
    else             cprintf("LOCK_SELFTEST_FAIL (%d failed)\n", g_fail);
    return g_fail;
}
