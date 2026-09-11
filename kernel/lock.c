#include "kernel/lock.h"
#include "kernel/irq.h"
#include "kernel/sched.h"
#include "kernel/ticker.h"
#include "kernel/time.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/hart.h"
#include "kernel/chan.h"
#include "kernel/klog.h"
#include "arch/atomic.h"
#include <stddef.h>

/* See kernel/include/kernel/lock.h for the rationale. */

void spinlock_init(spinlock_t *l) { if (l) l->word = 0; }

/* --- The leaf check (Y2, plan/phase31_concurrency_hierarchy.md) ---------
 *
 * Per-hart, for the reason kernel/lock.h gives: g_sched_lock is handed from
 * one task to another across ctx_switch(), so its acquire and its release
 * happen in different functions and different tasks but always on one hart.
 *
 * Safe without a lock of its own, which would be circular anyway: every write
 * below happens with interrupts already masked on this hart, and no other
 * hart reads this hart's slot. */
static uint8_t     g_spin_depth[MAX_HARTS];
static const char *g_spin_name[MAX_HARTS];
static const char *g_spin_site[MAX_HARTS];
static uint32_t    g_lock_faults;

/* The checker reports through printk_critical(), which reaches the UART
 * through that driver's own g_tx_batch_lock -- so reporting a violation takes
 * a spinlock, which would trip the check, which would report again. This flag
 * makes the check transparent for the duration of its own diagnostic.
 *
 * Per-hart and not a lock: it is only ever read and written by the hart it
 * belongs to, with interrupts masked. */
static bool        g_check_busy[MAX_HARTS];

/* Enough to see the pattern, few enough that a violation in a hot path cannot
 * fill the console faster than anyone can read it. The counter keeps counting
 * after the messages stop. */
#define LOCK_FAULT_REPORT_MAX 8

static void lock_fault(const char *what, const char *name, const char *site) {
    uint32_t h = hart_id();

    /* Before the counter, not after it. Reporting a fault means writing to
     * the console, and that path takes the UART driver's own spinlock -- so
     * every acquire the diagnostic itself makes arrives back here with a lock
     * already held. Those are not faults, they are the report.
     *
     * Counting them first and suppressing only the *message* is what the
     * first version of this did, and the selftest caught it immediately: one
     * deliberate violation reported "fault 55", because printk_critical()
     * had counted itself fifty-four times. A checker whose own count is
     * wrong is worse than no checker, since the number is exactly what a
     * test asserts on. */
    if (g_check_busy[h]) return;

    g_lock_faults++;
    if (g_lock_faults > LOCK_FAULT_REPORT_MAX) return;

    /* Snapshot before printing, because printing changes it.
     *
     * printk_critical() reaches the console through the UART driver, which
     * takes its own spinlock -- so by the second line of this message
     * g_spin_name[] names the console's lock rather than the one the caller
     * is actually holding. The first version read those globals inline and
     * reported every violation as "while holding &g_klog_lock", which is
     * both wrong and plausible enough to have been believed.
     *
     * The measurement has to be taken before the instrument perturbs it,
     * which is the same mistake as the fault counter above, one line apart. */
    const char *held_name = g_spin_name[h] ? g_spin_name[h] : "?";
    const char *held_site = g_spin_site[h] ? g_spin_site[h] : "?";
    unsigned    held_depth = (unsigned)g_spin_depth[h];

    g_check_busy[h] = true;
    /* Both locks named, which is the whole value of the diagnostic: "took"
     * says there is an ordering bug, and `A under B` says which two to go
     * and look at. */
    printk_critical("\n[Lock BUG] hart %u: %s", (unsigned)h, what);
    if (name) printk_critical(" %s (in %s())", name, site ? site : "?");
    printk_critical("\n[Lock BUG]   while holding %s, taken in %s().\n"
                    "[Lock BUG]   A spinlock_t is a leaf -- see kernel/lock.h. "
                    "Fault %u, depth %u.\n",
                    held_name, held_site,
                    (unsigned)g_lock_faults, held_depth);
    g_check_busy[h] = false;
}

/* --- No console output from inside a driver serve callback ---------------
 *
 * Per task, not per hart: a serve callback may block (uart's READ waits for a
 * keypress), and a task that blocks may resume on another hart. See
 * kernel/lock.h for why the rule exists and why it is checked here rather
 * than left to the wait-for graph, which only fires when a cycle actually
 * closes.
 *
 * Written only by the task itself, read by that task and by the diagnostic
 * below, one word at a time -- no lock. The failure mode of a torn read is a
 * pointer that is either the old name or the new one, and both are names of
 * this same driver.
 *
 * Known limitation, stated rather than papered over: the slot is indexed by
 * pid, so a driver task killed *inside* a serve callback leaves its slot set,
 * and a later task reusing that pid would be reported for a rule it is not
 * breaking. Driver tasks do not exit -- their bodies are infinite loops --
 * and the consequence of the false positive is one wrong line of diagnostic,
 * not a wrong behaviour. Clearing it from the exit path would mean a
 * scheduler hook for a case that cannot currently happen. */
static const char *g_serving[MAX_TASKS];

void lock_serve_enter(const char *what) {
    int me = sched_current_pid();
    if (me < 0 || me >= MAX_TASKS) return;
    g_serving[me] = what;
}

void lock_serve_leave(void) {
    int me = sched_current_pid();
    if (me < 0 || me >= MAX_TASKS) return;
    g_serving[me] = NULL;
}

const char *lock_serve_what(void) {
    int me = sched_current_pid();
    if (me < 0 || me >= MAX_TASKS) return NULL;
    return g_serving[me];
}

bool lock_check_may_console(void) {
    uint32_t h = hart_id();

    /* Before the counter and before anything else, for the reason
     * lock_fault() spells out at length: reporting this violation goes
     * through printk_critical(), which arrives back here. Guard first, count
     * second -- the other order made the first version of the spinlock
     * checker report "fault 55" for one violation. */
    if (g_check_busy[h]) return false;
    if (!sched_has_task()) return false;

    int me = sched_current_pid();
    if (me < 0 || me >= MAX_TASKS) return false;
    const char *what = g_serving[me];
    if (!what) return false;

    g_lock_faults++;
    if (g_lock_faults > LOCK_FAULT_REPORT_MAX) return true;

    g_check_busy[h] = true;
    printk_critical("\n[Lock BUG] task %d: console output from inside the '%s' "
                    "serve callback.\n"
                    "[Lock BUG]   cprintf() reaches the uart task through "
                    "chan_call(); a caller blocked on this endpoint closes the "
                    "cycle -- see kernel/lock.h. Fault %u.\n",
                    me, what, (unsigned)g_lock_faults);
    g_check_busy[h] = false;
    return true;
}

const char *lock_spin_held(void) { return g_spin_name[hart_id()]; }
const char *lock_spin_site(void) { return g_spin_site[hart_id()]; }
uint32_t    lock_faults(void)    { return g_lock_faults; }

bool lock_check_may_block_named(const char *what, const char *name,
                                const char *site) {
    uint32_t h = hart_id();
    if (!g_spin_depth[h]) return false;
    lock_fault(what, name, site);
    return true;
}

bool lock_check_may_block(const char *what) {
    return lock_check_may_block_named(what, NULL, NULL);
}

/* Shared body. `bottom` suppresses only the nested-take report -- everything
 * else, including the depth that makes blocking inside the section a
 * violation, is identical. See kernel/lock.h for why exactly one lock in this
 * kernel gets to pass true. */
static uintptr_t spin_lock_common(spinlock_t *l, const char *name,
                                  const char *site, bool bottom);

uintptr_t spin_lock_irqsave_bottom_at(spinlock_t *l, const char *name, const char *site) {
    return spin_lock_common(l, name, site, true);
}

uintptr_t spin_lock_irqsave_at(spinlock_t *l, const char *name, const char *site) {
    return spin_lock_common(l, name, site, false);
}

static uintptr_t spin_lock_common(spinlock_t *l, const char *name,
                                  const char *site, bool bottom) {
    uintptr_t flags = irq_save();
    uint32_t h = hart_id();

    /* Reported *before* the spin, not after: if this is the same lock, the
     * line below never returns, and the whole point is that the board says
     * why instead of simply stopping. That is the difference between phase
     * 31's F1 as it was found -- a dead machine after one printk -- and what
     * finding it should have cost. */
    if (g_spin_depth[h] && !bottom) lock_fault("took", name, site);

    for (;;) {
        if (arch_lock_try_acquire(&l->word)) {
            g_spin_depth[h]++;
            g_spin_name[h] = name;
            g_spin_site[h] = site;
            return flags;
        }

        /* Test-and-TEST-and-set: wait with plain loads, and only attempt the
         * atomic again once the word looks free (X7,
         * plan/phase23_multicore_scheduling.md).
         *
         * Spinning on the atomic itself is what this used to do, and the old
         * comment defended it -- "the holder is either running on another
         * hart, so waiting is exactly right". Waiting is right; *how* was
         * not. Every amoswap takes exclusive ownership of the cache line, so
         * a waiter hammering it can keep the holder from getting the line
         * back to release with. On QEMU the harts are host threads the OS
         * timeslices and it never shows. On RP2350's two cores and one bus
         * it deadlocks by starvation, and both cores stop: X7 reproduced
         * exactly that, with core 0 and core 1 recorded stuck inside printk
         * at the same instant.
         *
         * The backoff between reads touches nothing shared at all, which is
         * what gives the holder the bus back. */
        while (!arch_lock_looks_free(&l->word)) {
            for (volatile int spin = 0; spin < 16; spin++) { }
        }
    }
}

void spin_unlock_irqrestore_at(spinlock_t *l, uintptr_t flags) {
    uint32_t h = hart_id();
    /* Saturating rather than asserting: an unmatched release is a bug, but
     * one that would be made worse by a counter that wraps to 255 and then
     * reports every later acquire as nested. */
    if (g_spin_depth[h]) {
        g_spin_depth[h]--;
        if (!g_spin_depth[h]) { g_spin_name[h] = NULL; g_spin_site[h] = NULL; }
    }
    arch_lock_release(&l->word);
    irq_restore(flags);
}

bool spin_is_locked(const spinlock_t *l) { return l && l->word != 0; }

/* --- The wait-for graph (Y3) -------------------------------------------
 *
 * See kernel/lock.h for what is in it and why spinlocks are not. Moved here
 * from kernel/chan.c, which owned a channel-only version.
 *
 * Its own lock, and a leaf like every other spinlock_t: the critical sections
 * below walk a fixed array and call nothing. That is what lets waitfor_enter()
 * be called from chan_call_task() and from ylock_acquire() without either
 * needing to know the other exists. */
static spinlock_t g_waitfor_lock;
static int        g_waitfor[MAX_TASKS];
static bool       g_waitfor_ready;

static void waitfor_init_once(void) {
    if (g_waitfor_ready) return;
    for (int i = 0; i < MAX_TASKS; i++) g_waitfor[i] = -1;
    g_waitfor_ready = true;
}

/* Caller holds g_waitfor_lock. Walks from `target` and reports whether it can
 * reach `me`; the guard bounds it at the table size, so a graph corrupted into
 * a loop that does not contain `me` terminates rather than spinning. */
static bool waitfor_reaches(int me, int target) {
    int cur = target;
    for (int guard = 0; cur >= 0 && cur < MAX_TASKS && guard < MAX_TASKS; guard++) {
        if (cur == me) return true;
        cur = g_waitfor[cur];
    }
    return false;
}

bool waitfor_enter(int me, int target) {
    if (me < 0 || me >= MAX_TASKS || target < 0 || target >= MAX_TASKS) return true;

    uintptr_t f = spin_lock_irqsave(&g_waitfor_lock);
    waitfor_init_once();
    bool cycle = waitfor_reaches(me, target);
    if (!cycle) g_waitfor[me] = target;
    spin_unlock_irqrestore(&g_waitfor_lock, f);
    return !cycle;
}

void waitfor_leave(int me) {
    if (me < 0 || me >= MAX_TASKS) return;
    uintptr_t f = spin_lock_irqsave(&g_waitfor_lock);
    g_waitfor[me] = -1;
    spin_unlock_irqrestore(&g_waitfor_lock, f);
}

void waitfor_report_cycle(const char *what, int me, int target) {
    uint32_t h = hart_id();
    if (g_check_busy[h]) return;
    g_lock_faults++;
    if (g_lock_faults > LOCK_FAULT_REPORT_MAX) return;
    g_check_busy[h] = true;
    printk_critical("\n[Lock BUG] hart %u: %s -- task %d waiting for task %d "
                    "closes a wait-for cycle.\n"
                    "[Lock BUG]   Nothing can refuse this one; it will hang. "
                    "See kernel/lock.h. Fault %u.\n",
                    (unsigned)h, what, me, target, (unsigned)g_lock_faults);
    g_check_busy[h] = false;
}

int waitfor_target(int pid) {
    if (pid < 0 || pid >= MAX_TASKS) return -1;
    return g_waitfor[pid];
}

void ylock_init(ylock_t *l) {
    if (!l) return;
    l->word = 0;
    l->owner = -1;
    l->depth = 0;
}

void ylock_acquire_at(ylock_t *l, const char *name, const char *site) {
    (void)name; (void)site;
    /* A ylock may be held across a block -- that is what it is for -- but it
     * may not be taken *under* a spinlock, because the yield below would then
     * happen with that spinlock held. Reported, not refused: the caller has
     * no alternative to offer and refusing would break it worse. */
    lock_check_may_block_named("took ylock", name, site);
    /* The context, not the pid -- for the same reason the console lock needs it:
     * -1 is this lock's "free" marker (see ylock_owner()), so a hart with no
     * task must not identify as -1 or it would read its own held lock as
     * unowned. sched_context_id() is never -1. Contention from such a hart
     * spins rather than yields, which sched_yield() below already does for
     * it; a bring-up hart taking a ylock is rare and short by construction. */
    int me = sched_context_id();
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
            waitfor_leave(me);
            return;
        }

        if (arch_lock_try_acquire(&l->word)) {
            l->owner = me;
            l->depth = 1;
            irq_restore(f);
            waitfor_leave(me);
            return;
        }

        /* Held by someone else. Record the edge before yielding (Y3): this
         * task is now waiting for whoever holds the lock, and that is a
         * wait-for edge exactly like a chan_call()'s. Recording it is what
         * lets §0.3's cycle be seen -- a task holding this ylock and calling
         * a task that wants it is refused *at the chan_call*, because that
         * call can see the edge this line just added.
         *
         * Reported and not refused when the cycle closes here instead:
         * ylock_acquire() has no failure to return, and a caller given one
         * would have nothing useful to do with it. The report names it at the
         * moment it happens, which is the difference between a livelock
         * anyone can diagnose and one nobody can.
         *
         * Edge dropped on every exit from the loop, including the acquiring
         * ones, so a lock that is obtained leaves nothing behind. */
        int holder = l->owner;
        irq_restore(f);

        if (me >= 0 && me < MAX_TASKS && !waitfor_enter(me, holder)) {
            waitfor_report_cycle("ylock_acquire()", me, holder);
        }
        sched_yield();
        waitfor_leave(me);
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
/* Counted rather than written into the summary by hand: the literal "7/7" it
 * used to print stopped being true the moment Y2 added two checks, and a
 * count that can drift from what ran is a count nobody should trust. */
static int g_checks;

/* --- Y3's deliberate cycle -------------------------------------------------
 *
 * The §0.3 cycle in its exact shape: a task holds a lock, another task waits
 * for that lock, and the holder then calls the waiter. Built so that a
 * *working* checker never blocks -- the call is refused before it can -- and
 * so the precondition is asserted first, which keeps a regression from
 * turning into a silent hang in a 363-test suite. */
static ylock_t g_cycle_lock;
static volatile bool g_cycle_waiting;
static volatile bool g_cycle_done;

static void cycle_waiter_task(void *arg) {
    (void)arg;
    g_cycle_waiting = true;
    ylock_acquire(&g_cycle_lock);   /* held by the selftest: records the edge */
    ylock_release(&g_cycle_lock);
    g_cycle_done = true;
}

static void check(const char *what, bool ok) {
    cprintf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    g_checks++;
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
    g_checks = 0;
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

        /* Deliberately NOT asserting depth==0 / owner==-1 here.
         *
         * That assertion was in this test and it was wrong -- valid only on a
         * kernel where nothing else can run between the release and the next
         * statement. With a second hart the contender is already inside the
         * lock by the time this line executes, so the snapshot legitimately
         * reads depth 1 owned by someone else, and the test failed on a
         * kernel that was behaving perfectly (found by phase 23's X1,
         * 2026-09-04).
         *
         * It was also redundant: the release is proven by the contender
         * getting in, which is checked below and cannot happen unless the
         * lock really was handed over. An intermediate state that a
         * concurrent system may change at any moment is not something a test
         * is entitled to observe. */
        for (int i = 0; i < 200 && spawned && !g_contender_done; i++) sched_yield();
        bool contender_got_in = g_contender_acquired && g_contender_done;

        check("ylock: the owner re-enters, depth counts up",
              owned_once && reentered);
        check("ylock: a different task is scheduled but stays out while held",
              spawned && contender_scheduled && kept_out && still_held && kept_out_at_depth_1);
        check("ylock: the last release frees it and the waiter proceeds",
              spawned && contender_got_in);
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

    /* --- 5. the leaf checker refuses what it is supposed to refuse ----- */
    {
        /* Y2's done-condition: a deliberate violation must produce a named
         * diagnostic rather than a hang. Two locks are taken out of order and
         * chan_call() is made with one held, and the fault counter is read
         * around each -- the console line is for a human, the counter is what
         * a test can assert on.
         *
         * Two *different* locks, deliberately. Taking the same one twice
         * would report and then spin forever on a lock this hart holds, which
         * is a correct outcome and an untestable one. */
        spinlock_t outer, inner;
        spinlock_init(&outer);
        spinlock_init(&inner);

        uint32_t before = lock_faults();
        uintptr_t fo = spin_lock_irqsave(&outer);
        uintptr_t fi = spin_lock_irqsave(&inner);   /* the violation */
        spin_unlock_irqrestore(&inner, fi);
        spin_unlock_irqrestore(&outer, fo);
        bool nested_caught = (lock_faults() == before + 1);

        /* And that it goes quiet again once the lock is released: a checker
         * that reports unconditionally would pass the line above too. */
        uint32_t before_clean = lock_faults();
        uintptr_t fc = spin_lock_irqsave(&outer);
        spin_unlock_irqrestore(&outer, fc);
        bool clean_is_quiet = (lock_faults() == before_clean);

        check("leaf check: a second spinlock is reported, one clean take is not",
              nested_caught && clean_is_quiet);

        /* chan_call() with a spinlock held must be refused, not merely
         * counted -- refusal is already its contract, and every caller in the
         * tree handles -1 by falling back to direct access. The endpoint name
         * is the console's, which exists on every target. */
        chan_endpoint_t *ep = chan_lookup("console");
        if (!ep) {
            cprintf("  [skip] chan_call under a lock: no 'console' endpoint\n");
        } else {
            uint8_t req[1] = { 0 }, resp[8];
            uint32_t before_call = lock_faults();
            uintptr_t f2 = spin_lock_irqsave(&outer);
            int n = chan_call(ep, req, sizeof(req), resp, sizeof(resp));
            spin_unlock_irqrestore(&outer, f2);
            check("leaf check: chan_call() with a spinlock held is refused",
                  n == -1 && lock_faults() == before_call + 1);
        }
    }

    /* --- 6. one graph: a lock edge and a channel edge close a cycle ---- */
    {
        /* §0.3, which the old channel-only graph could not see: task A holds
         * lock L, calls task B, and B tries to take L. Here the halves are
         * assembled in the order that lets the *call* be the one refused,
         * which is the half that can refuse. */
        ylock_init(&g_cycle_lock);
        g_cycle_waiting = false;
        g_cycle_done = false;

        int me = sched_current_pid();
        ylock_acquire(&g_cycle_lock);

        int wpid = task_create("cyclewait", cycle_waiter_task, NULL);
        bool spawned = (wpid >= 0);

        /* Let the waiter reach ylock_acquire() and record its edge.
         *
         * The edge exists only while the waiter is inside its yield, so this
         * samples a window rather than a state, and the sampling has to be
         * generous enough not to depend on the shape of the run queue. It was
         * 200 bare sched_yield()s, which was enough until Y5c put klogd in
         * the rotation and then failed about one run in three on two harts --
         * a fragile measurement of a property that had not changed.
         *
         * task_sleep_ms() rather than more yields: a yield hands off within
         * this hart's ready set, and what is needed is for the *waiter* to be
         * scheduled, which on the other hart may mean waiting for real time
         * to pass. Bounded at ~250 ms, which is invisible in a suite run. */
        bool edge_recorded = false;
        for (int i = 0; i < 500 && spawned && !edge_recorded; i++) {
            edge_recorded = (waitfor_target(wpid) == me);
            if (!edge_recorded) { sched_yield(); if ((i % 20) == 19) task_sleep_ms(10); }
        }
        check("wait-for graph: a ylock wait is an edge, like a channel call",
              edge_recorded);

        /* The cycle itself. Guarded by the precondition above: without the
         * edge there is nothing to detect, and calling anyway would block on
         * a task that is not serving. */
        if (edge_recorded) {
            static uint8_t creq[8], cresp[8];
            if (chan_register_task("lockcycle", wpid, creq, sizeof(creq),
                                   cresp, sizeof(cresp)) == 0) {
                chan_endpoint_t *cep = chan_lookup("lockcycle");
                uint8_t req[1] = { 1 }, resp[1];
                int n = cep ? chan_call(cep, req, 1, resp, sizeof(resp)) : 0;
                check("one graph: calling a task that waits for our lock is refused",
                      n == -1);
            } else {
                cprintf("  [skip] cycle call: endpoint table full\n");
            }
        }

        /* Let the waiter through and out, so nothing is left holding or
         * waiting when the selftest returns. */
        ylock_release(&g_cycle_lock);
        for (int i = 0; i < 400 && spawned && !g_cycle_done; i++) sched_yield();
        check("wait-for graph: the edge clears and the waiter completes",
              spawned && g_cycle_done && waitfor_target(wpid) == -1);
    }

    /* --- 7. no console output from inside a driver serve callback ----- */
    {
        /* The mark driver_task.c sets around every serve callback. Set it by
         * hand here rather than going through a real driver task: the claim
         * under test is the checker's, and a test that needed a driver to
         * demonstrate it would be testing two things at once.
         *
         * printk() *inside* the bracket is the violation; check() itself
         * prints, so it is called after the mark is cleared -- otherwise the
         * report would be the thing being reported on, which is the mistake
         * Y2's fault counter made and this file exists to remember. */
        /* Y5c narrowed what this rule covers, and the two halves are now
         * opposite. printk() from inside a serve callback is *safe*: it
         * appends to the log ring and returns, reaching no blocking
         * primitive at all. cprintf() is not: it still goes to the uart task
         * through chan_call(), and a caller blocked on this endpoint closes
         * the cycle. Asserting both directions is the point -- a checker that
         * still fired on printk() would be reporting a rule that no longer
         * exists. */
        uint32_t before_printk = lock_faults();
        lock_serve_enter("selftest");
        bool marked = (lock_serve_what() != NULL);
        printk("[KlogTest] printk from inside a serve callback is fine now\n");
        lock_serve_leave();
        bool printk_is_quiet = (lock_faults() == before_printk);

        uint32_t before_cprintf = lock_faults();
        lock_serve_enter("selftest");
        cprintf("  (deliberate: console output from inside a serve callback)\n");
        lock_serve_leave();
        bool cprintf_caught = (lock_faults() == before_cprintf + 1);

        check("serve callback: printk() is safe, cprintf() is still reported",
              marked && printk_is_quiet && cprintf_caught &&
              lock_serve_what() == NULL);
    }

    /* --- 8. Y5b: the log record is the atomic unit ---------------------- */
    {
        /* printk() no longer emits character by character under a lock held
         * across the whole message; it formats into a buffer on its own stack
         * and appends one record under g_klog_lock. The property that buys is
         * that a message is stored whole and comes back whole -- asserted end
         * to end here rather than inferred from the spinlock, because the
         * store and the readback are new code and the spinlock is not. */
        char back[300];
        uint64_t before = klog_total();
        printk("[KlogTest] %s\n",
               "0123456789abcdef0123456789abcdef0123456789abcdef"
               "0123456789abcdef0123456789abcdef0123456789abcdef");
        uint32_t n = klog_read(before, back, sizeof(back) - 1);
        back[n] = '\0';

        /* Contiguous: the marker, then 96 filler characters with nothing
         * spliced into them. A torn record would put another task's text or a
         * header's bytes in the middle. */
        bool whole = false;
        for (uint32_t i = 0; i + 10 < n; i++) {
            if (back[i] != '[' || back[i+1] != 'K') continue;
            uint32_t j = i + 11;                 /* past "[KlogTest] " */
            uint32_t run = 0;
            while (j + run < n && run < 96 &&
                   ((back[j+run] >= '0' && back[j+run] <= '9') ||
                    (back[j+run] >= 'a' && back[j+run] <= 'f'))) run++;
            if (run == 96) { whole = true; break; }
        }

        /* And that the timestamp survived the round trip through four binary
         * bytes: the record was emitted with a bracketed tag, so it renders
         * with one. */
        bool stamped = (n > 0 && back[0] == '[' && back[6] == '.');

        check("log record: stored whole, read back whole, timestamp rebuilt",
              whole && stamped);

        /* Truncation is visible and counted, not silent. */
        uint32_t trunc_before = klog_truncations();
        static char toolong[KLOG_REC_MAX + 64];
        for (uint32_t i = 0; i < sizeof(toolong) - 1; i++) toolong[i] = 'x';
        toolong[sizeof(toolong) - 1] = '\0';
        printk("[KlogTest] %s\n", toolong);
        check("log record: an over-long message is truncated and counted",
              klog_truncations() == trunc_before + 1);
    }

    /* --- 9. Y5c: the producer never blocks, and says what it lost -------- */
    {
        /* A burst nobody can drain, forced rather than hoped for.
         *
         * spin_lock_irqsave() masks interrupts on this hart, so klogd cannot
         * be scheduled for the duration -- and klog_emit() skips the wake
         * anyway while a spinlock is held, because task_unblock() would take
         * g_sched_lock and nesting that is what Y2 refuses. So the ring fills,
         * evicts, and the consumer finds a hole where its cursor used to be.
         *
         * This is also the assertion that printk() is callable from a context
         * that could not possibly block: with a spinlock held, 400 times. */
        static spinlock_t burst;
        spinlock_init(&burst);

        uint32_t gaps_before = klog_gaps();
        uintptr_t bf = spin_lock_irqsave(&burst);
        for (int i = 0; i < 400; i++) {
            printk("[KlogBurst] record %d of 400, filling the ring with no consumer running\n", i);
        }
        spin_unlock_irqrestore(&burst, bf);

        /* One ordinary printk to send the wake the burst could not: every
         * emit inside the loop above skipped it, because a spinlock was held
         * and task_unblock() takes g_sched_lock. Without this, klogd sleeps
         * out its full idle period first -- which is correct behaviour and
         * was the first version of this test's bug, since sched_yield() does
         * not advance a sleeping task's deadline. */
        printk("[KlogBurst] burst over\n");
        for (int i = 0; i < 40 && klog_gaps() == gaps_before; i++) task_sleep_ms(5);

        check("log burst: the producer never blocked, and the loss is reported",
              klog_gaps() == gaps_before + 1 && klog_gap_bytes() > 0);
    }

    /* The fault total includes the three this selftest caused on purpose,
     * which is why it is printed rather than asserted to be zero: a reader
     * wants to know whether anything *else* has tripped the checker since
     * boot. */
    cprintf("  lock-hierarchy faults since boot: %u (3 of them deliberate, above)\n",
            (unsigned)lock_faults());
    if (g_fail == 0) cprintf("LOCK_SELFTEST_OK (%d/%d)\n", g_checks, g_checks);
    else             cprintf("LOCK_SELFTEST_FAIL (%d of %d failed)\n", g_fail, g_checks);
    return g_fail;
}