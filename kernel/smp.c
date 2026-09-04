#include "lugalos_config.h"
#include "kernel/hart.h"
#include "kernel/lock.h"
#include "kernel/sched.h"
#include "kernel/irq.h"
#include "kernel/ticker.h"
#include "kernel/time.h"
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

#if CONFIG_ENABLE_SMP && defined(CONFIG_BOARD_RP2350)

/* RP2350's SIO FIFO, the mailbox the bootrom listens on while core 1 waits.
 * Offsets and status bits from the SDK's own register header
 * (src/rp2350/hardware_regs/include/hardware/regs/sio.h), not derived. */
#define SIO_BASE_ADDR   0xd0000000UL
#define SIO_FIFO_ST     (*(volatile uint32_t *)(SIO_BASE_ADDR + 0x50))
#define SIO_FIFO_WR     (*(volatile uint32_t *)(SIO_BASE_ADDR + 0x54))
#define SIO_FIFO_RD     (*(volatile uint32_t *)(SIO_BASE_ADDR + 0x58))
#define FIFO_ST_VLD     (1u << 0)   /* data waiting to be read */
#define FIFO_ST_RDY     (1u << 1)   /* space available to write */

extern void core1_entry(void);
extern uint32_t _stack1_top;

/* X3's evidence, and deliberately the whole of what core 1 does.
 *
 * The milestone's bar is "proof core 1 executed anything at all (e.g. a
 * counter core 1 increments that core 0 reads back)". Meeting exactly that
 * bar first is not timidity, it is the only way to tell two very different
 * failures apart: a launch handshake that never starts the core, and a core
 * that starts and then dies inside the kernel it was pointed at.
 *
 * The first attempt skipped this step and sent core 1 straight into
 * secondary_main() -- trap_init(), the ticker, the scheduler, printk. It
 * wedged, and because everything downstream is silent when it does, there
 * was no way to say which half had failed. This counter separates them: if
 * it moves, the bootrom handshake, the stack, and core 1's first
 * instructions are all good, and anything still broken is above that line.
 *
 * In .bss, which core 0 cleared at boot long before smpstart runs, so a
 * non-zero value can only have been written by core 1. */
volatile uint32_t g_core1_ticks;

/* Core 1's entire program. No printk (the console path blocks and would
 * reach core 0's pinned uart task), no scheduler (core 1 has no task yet),
 * no interrupts (mstatus is zeroed in core1_entry). Just proof of life. */
void core1_probe_main(void) {
    for (;;) {
        g_core1_ticks++;
    }
}

/* Hazard3's equivalent of Arm's SEV: `slt x0, x0, x1` is a hint encoding --
 * a no-op on a standard RISC-V core, decoded by Hazard3 as "unblock". Taken
 * from the SDK's __hazard3_unblock() rather than invented; core 1 may be
 * waiting for FIFO space in the blocked state, and nothing else wakes it. */
static inline void hazard3_unblock(void) {
    __asm__ __volatile__("slt x0, x0, x1" ::: "memory");
}

static void fifo_drain(void) {
    while (SIO_FIFO_ST & FIFO_ST_VLD) (void)SIO_FIFO_RD;
}

/* Launches core 1 (X3).
 *
 * The sequence and its handshake are the bootrom's, transcribed from the
 * SDK's multicore_launch_core1_raw(): six words, each echoed back by core 1,
 * and any mismatch restarts from the beginning -- which is what makes it
 * robust against a FIFO holding stale data from a previous attempt. The
 * zeros are resynchronisation points, so the read FIFO is drained and core 1
 * unblocked before each one.
 *
 * The third value is mtvec on RISC-V where it is VTOR on Arm; the SDK
 * branches on __riscv for exactly that. Getting it from the CSR rather than
 * from the symbol means core 1 starts with whatever core 0 is actually
 * using.
 *
 * Returns false rather than spinning forever if core 1 does not answer --
 * a board whose second core never comes up must still boot. */
static bool smp_launch_core1(void) {
    uintptr_t mtvec;
    __asm__ __volatile__("csrr %0, mtvec" : "=r"(mtvec));

    const uint32_t seq[6] = {
        0, 0, 1,
        (uint32_t)(uintptr_t)mtvec,
        (uint32_t)(uintptr_t)&_stack1_top,
        (uint32_t)(uintptr_t)core1_entry,
    };

    unsigned i = 0;

    /* ONE budget for the whole handshake, decremented by every wait.
     *
     * The first version of this had a 2,000,000-iteration outer guard whose
     * body contained two inner waits of 100,000 spins each -- a product of
     * 4e11, which is "bounded" only in the sense that it terminates before
     * the heat death of the universe. On hardware it wedged the board
     * silently at boot and cost a BOOTSEL recovery. A budget that is not a
     * single counter is not a budget. */
    uint32_t budget = 2000000u;

    while (i < 6 && budget > 0) {
        uint32_t cmd = seq[i];
        if (cmd == 0) {
            fifo_drain();
            hazard3_unblock();
        }

        while (!(SIO_FIFO_ST & FIFO_ST_RDY) && --budget) { }
        if (budget == 0) break;
        SIO_FIFO_WR = cmd;
        hazard3_unblock();

        while (!(SIO_FIFO_ST & FIFO_ST_VLD) && --budget) { }
        if (budget == 0) break;
        uint32_t resp = SIO_FIFO_RD;

        i = (resp == cmd) ? i + 1 : 0;
        budget--;
    }
    return i == 6;
}
int smp_start_secondary(void) {
    uint32_t before = g_core1_ticks;
    printk("[SMP] core1 ticks before launch: %lu\n", (unsigned long)before);

    if (!smp_launch_core1()) {
        printk("[SMP] core 1 did not answer the launch handshake (still parked)\n");
        return -1;
    }
    printk("[SMP] launch handshake completed\n");

    /* A bounded wait, on the microsecond timer rather than a spin count, so
     * the figure means something. Core 1 has nothing to do but increment. */
    uint64_t deadline = time_get_us() + 50000;   /* 50 ms */
    while (time_get_us() < deadline) { }

    uint32_t after = g_core1_ticks;
    printk("[SMP] core1 ticks after 50ms: %lu (delta %lu)\n",
           (unsigned long)after, (unsigned long)(after - before));
    if (after != before) {
        printk("[SMP] CORE1_ALIVE -- a second Hazard3 core is executing our code\n");
        return 0;
    }
    printk("[SMP] CORE1_SILENT -- handshake completed but the counter never moved\n");
    return -1;
}
#endif /* CONFIG_ENABLE_SMP && CONFIG_BOARD_RP2350 */

void smp_release_secondaries(void) {
#if CONFIG_ENABLE_SMP
    /* Everything a secondary will touch -- .bss cleared, g_harts zeroed, the
     * scheduler and its lock initialised -- must be visible before the write
     * that lets it proceed. The fence is the entire ordering guarantee here;
     * without it the store could be observed by hart 1 before the stores it
     * is meant to publish. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_smp_release = SMP_RELEASE_MAGIC;
#if defined(CONFIG_BOARD_RP2350)
    /* Deliberately NOT launched here.
     *
     * RP2350's core 1 is inside the bootrom and has never executed this
     * image -- established by probe on real silicon, see boot_header.S -- so
     * starting it means a FIFO handshake with hardware, at boot, before
     * there is a console to say anything on. The first version of this ran
     * at boot, wedged, and produced a completely silent board that needed a
     * BOOTSEL recovery to reflash.
     *
     * So the launch is an explicit `smpstart` from the shell instead. A
     * board that boots is a board that can be reflashed, and X3's own bar --
     * proof that core 1 executed anything at all -- does not require it to
     * happen automatically. Making it a boot step is a later decision, once
     * the handshake has a track record. */
    printk("[SMP] core 1 is parked in the bootrom; `smpstart` launches it\n");
#else
    printk("[SMP] Secondary harts released (max %d)\n", MAX_HARTS);
#endif
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
