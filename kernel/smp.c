#include "lugalos_config.h"
#include "kernel/hart.h"
#include "kernel/lock.h"
#include "kernel/sched.h"
#include "kernel/irq.h"
#include "kernel/ticker.h"
#include "kernel/printk.h"
#include "arch/trap.h"
#include "arch/vmm.h"
#include "kernel/console.h"
#include <stddef.h>
#include <stdbool.h>

/* Secondary hart bring-up (X1, plan/phase23_multicore_scheduling.md).
 *
 * All of this is inert unless CONFIG_ENABLE_SMP. With the gate off,
 * arch/riscv/common/entry.S parks secondary harts exactly as it did before
 * phase 23, smp_release_secondaries() does nothing, and secondary_main() is
 * never reached -- so every existing board persona boots on one hart with
 * this file compiled in and doing nothing, which is what makes the gate
 * worth having rather than a second build to maintain.
 */

/* Read by entry.S's .Lsecondary_wait spin loop. Lives in .bss, which is why
 * the protocol needs a magic value rather than a flag -- see
 * SMP_RELEASE_MAGIC in kernel/hart.h. */
volatile uint32_t g_smp_release;

static spinlock_t g_smp_lock;
static unsigned   g_online = 1;   /* the primary is online by definition */

unsigned smp_harts_online(void) {
    uintptr_t f = spin_lock_irqsave(&g_smp_lock);
    unsigned n = g_online;
    spin_unlock_irqrestore(&g_smp_lock, f);
    return n;
}

void smp_release_secondaries(void) {
#if CONFIG_ENABLE_SMP
    /* Everything a secondary will touch -- .bss cleared, g_harts zeroed, the
     * scheduler and its lock initialised -- must be visible before the write
     * that lets it proceed. The fence is the entire ordering guarantee here;
     * without it the store could be observed by hart 1 before the stores it
     * is meant to publish. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_smp_release = SMP_RELEASE_MAGIC;
    printk("[SMP] Secondary harts released (max %d)\n", MAX_HARTS);
#endif
}

#if CONFIG_ENABLE_SMP
void secondary_main(void) {
    /* First, before anything that touches a kernel address: join the address
     * space the primary built. satp is per-hart, so until this runs we are in
     * bare mode while hart 0 translates. See vmm_secondary_init(). */
    /* First, before anything that touches a kernel address: join the address
     * space the primary built. satp is per-hart, so until this runs we are in
     * bare mode while hart 0 translates. See vmm_secondary_init(). */
    vmm_secondary_init();

    /* Per-hart interrupt state. trap_init() writes this hart's own PLIC
     * context since §6.1's fix; before it, this call would have
     * reconfigured hart 0's. */
    trap_init();

    /* This hart's own preemption deadline. stimecmp and sie are per-hart
     * CSRs, so arming is genuinely local -- what is NOT local is the tick
     * *rate*, which the primary measured once and this reuses. */
    ticker_arm_this_hart();

    /* Become a task, so there is something to switch away from. */
    int pid = sched_secondary_init();
    if (pid < 0) {
        printk("[SMP] hart %u: no free task slot; parking\n", hart_id());
        for (;;) { __asm__ __volatile__("wfi"); }
    }

    {
        uintptr_t f = spin_lock_irqsave(&g_smp_lock);
        g_online++;
        spin_unlock_irqrestore(&g_smp_lock, f);
    }
    printk("[SMP] hart %u online as pid %d\n", hart_id(), pid);

    /* Interrupts on, then idle: pull whatever the shared ready queue has.
     *
     * sched_yield() returns immediately when nothing else can run, so this
     * is a spin rather than a halt. That is deliberate for X1 -- a wfi here
     * would need a cross-hart wakeup on every task_unblock(), which is real
     * work and not what this milestone is proving. It costs a busy core on
     * an idle system, which on QEMU is a host thread and on RP2350 would be
     * power; X2 or later can make it wfi once there is a reason to. */
    irq_restore(IRQ_ENABLE_BIT);
    for (;;) {
        sched_yield();
    }
}
#endif /* CONFIG_ENABLE_SMP */

/* --- X1's verification --------------------------------------------------
 *
 * The milestone asks for proof that two tasks are *demonstrably* running
 * concurrently, "not merely interleaved". Those are different claims and
 * only one of them is easy: interleaving is what a single preemptive hart
 * already does, and any test that merely observes two tasks making progress
 * cannot tell the two apart.
 *
 * So this asserts two things that interleaving on one hart cannot produce:
 *
 *   1. Work ran on more than one hart. Each worker ORs its own hart id into
 *      a mask, so the evidence is direct -- a bit set by code that was
 *      executing on hart 1 -- rather than inferred from timing.
 *   2. A counter incremented under the lock lost nothing. On one hart this
 *      passes trivially; under real contention it is the whole claim of
 *      phase 22, checked for the first time against an actual second hart
 *      rather than by argument.
 *
 * A second counter is incremented *without* the lock, deliberately, and its
 * loss is reported rather than asserted. It is the contrast that makes the
 * first counter mean something: if both counters come out exact, there was
 * no contention and result (2) proved nothing. Not asserted because "a race
 * must lose at least one update" is a probabilistic claim, and a test that
 * fails when the machine happens to be fast is worse than one that reports.
 */

#if !CONFIG_ENABLE_SMP
/* Without the gate there is no second hart, so this cannot make its central
 * claim. It says so rather than creating four workers and reporting "ran on
 * 1 hart" as a failure -- a command that fails by design on every default
 * build teaches the reader to ignore it. */
int smp_selftest(void) {
    cprintf("SMP concurrency selftest:\n");
    cprintf("  [skip] this build has CONFIG_ENABLE_SMP=0; there is no second hart\n");
    cprintf("         (cmake --preset rv64-smp, then run under qemu -smp 2)\n");
    cprintf("SMP_SELFTEST_SKIPPED\n");
    return 0;
}
#else

#define SMPTEST_WORKERS 4
#define SMPTEST_ITERS   20000

static spinlock_t        g_smptest_lock;
static volatile uint32_t g_smptest_locked;
static volatile uint32_t g_smptest_unlocked;
static volatile uint32_t g_smptest_harts;
static volatile int      g_smptest_done;

static void smptest_worker(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < SMPTEST_ITERS; i++) {
        uintptr_t f = spin_lock_irqsave(&g_smptest_lock);
        g_smptest_locked++;
        g_smptest_harts |= 1u << hart_id();
        spin_unlock_irqrestore(&g_smptest_lock, f);

        /* Read-modify-write through a local, with no exclusion at all. Split
         * this way on purpose: it widens the window a competing hart has to
         * land in, so the contrast above is visible rather than theoretical. */
        uint32_t v = g_smptest_unlocked;
        g_smptest_unlocked = v + 1;
    }
    uintptr_t f = spin_lock_irqsave(&g_smptest_lock);
    g_smptest_done++;
    spin_unlock_irqrestore(&g_smptest_lock, f);
}

int smp_selftest(void) {
    int fail = 0;
    cprintf("SMP concurrency selftest:\n");

    spinlock_init(&g_smptest_lock);
    g_smptest_locked = 0;
    g_smptest_unlocked = 0;
    g_smptest_harts = 0;
    g_smptest_done = 0;

    int made = 0;
    for (int i = 0; i < SMPTEST_WORKERS; i++) {
        if (task_create("smpw", smptest_worker, NULL) >= 0) made++;
    }
    if (made != SMPTEST_WORKERS) {
        cprintf("  [FAIL] could only create %d of %d workers\n", made, SMPTEST_WORKERS);
        cprintf("SMP_SELFTEST_FAIL (1 failed)\n");
        return 1;
    }

    /* Bounded: a worker that never finishes must not hang the shell. */
    for (int guard = 0; guard < 20000000 && g_smptest_done < made; guard++) {
        sched_yield();
    }

    uint32_t expect = (uint32_t)made * SMPTEST_ITERS;
    unsigned harts = 0;
    for (unsigned b = 0; b < MAX_HARTS; b++) if (g_smptest_harts & (1u << b)) harts++;

    cprintf("  workers=%d iters=%u  locked=%u (want %u)  unlocked=%u (lost %u)  harts=%u\n",
            made, (unsigned)SMPTEST_ITERS, (unsigned)g_smptest_locked, (unsigned)expect,
            (unsigned)g_smptest_unlocked, (unsigned)(expect - g_smptest_unlocked), harts);

    bool all_done = (g_smptest_done == made);
    cprintf("  [%s] every worker finished\n", all_done ? "ok" : "FAIL");
    if (!all_done) fail++;

    bool exact = (g_smptest_locked == expect);
    cprintf("  [%s] no update lost through the lock\n", exact ? "ok" : "FAIL");
    if (!exact) fail++;

    /* The claim the milestone actually rides on. */
    bool concurrent = (harts >= 2);
    cprintf("  [%s] work executed on %u hart(s), not one\n",
            concurrent ? "ok" : "FAIL", harts);
    if (!concurrent) fail++;

    if (g_smptest_unlocked == expect) {
        cprintf("  [note] the unlocked counter lost nothing -- there was little or no\n"
                "         real contention, so the exactness above proves less than it looks\n");
    }

    if (fail == 0) cprintf("SMP_SELFTEST_OK (3/3)\n");
    else           cprintf("SMP_SELFTEST_FAIL (%d failed)\n", fail);
    return fail;
}

#endif /* CONFIG_ENABLE_SMP */
