#include "lugalos_config.h"
#include "kernel/hart.h"
#include "kernel/lock.h"
#include "arch/atomic.h"
#include "kernel/sched.h"
#include "kernel/irq.h"
#include "kernel/ticker.h"
#include "kernel/time.h"
#include "kernel/printk.h"
#include "arch/trap.h"
#include "arch/vmm.h"
#include "arch/rp2350_bootrom.h"
#include "kernel/console.h"
#include "kernel/meminfo.h"
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

/* Published by a secondary once it has a task, read by the primary so the
 * secondary does not have to print during bring-up. -1 until then. */
volatile int g_secondary_pid = -1;

static spinlock_t g_smp_lock;
static unsigned   g_online = 1;   /* the primary is online by definition */

unsigned smp_harts_online(void) {
    uintptr_t f = spin_lock_irqsave(&g_smp_lock);
    unsigned n = g_online;
    spin_unlock_irqrestore(&g_smp_lock, f);
    return n;
}

/* Bring-up progress, recorded where it survives the core dying (X7).
 * RP2350 only -- it writes linker/rp2350.ld's .smpmark, which no boot path
 * clears. A no-op elsewhere, so secondary_main() below reads the same on
 * every target. */
#if CONFIG_ENABLE_SMP && defined(CONFIG_BOARD_RP2350)
void smp_mark(uint32_t step);
#else
static inline void smp_mark(uint32_t step) { (void)step; }
#endif

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
extern uint32_t _stack1_bottom;
extern uint32_t _stack1_top;

/* Core 1's stack high-water mark, in bytes, or 0 if it has never run.
 *
 * The whole reason X7's first hardware attempt took the board down was a
 * stack sized by argument rather than by measurement -- 4 KB in SCRATCH_Y,
 * chosen when core 1's entire program was a counter, against a chain that
 * core 0 measures at 8372 bytes. So core 0 paints this stack before the
 * launch and anything can read back what core 1 actually used, the same way
 * kernel/meminfo.c already reports the boot stack and `ps` reports each
 * task's. A number, not a belief. */
uint32_t smp_core1_stack_used(void) {
    const uintptr_t *p   = (const uintptr_t *)&_stack1_bottom;
    const uintptr_t *top = (const uintptr_t *)&_stack1_top;
    if (p[0] != STACK_POISON_WORD && p[0] == 0) return 0;  /* never painted */
    while (p < top && *p == STACK_POISON_WORD) p++;
    return (uint32_t)((uintptr_t)top - (uintptr_t)p);
}

static void smp_paint_core1_stack(void) {
    uintptr_t *p   = (uintptr_t *)&_stack1_bottom;
    uintptr_t *top = (uintptr_t *)&_stack1_top;
    while (p < top) *p++ = STACK_POISON_WORD;
}

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

/* X7: which program core 1 runs, chosen by core 0 before the handshake and
 * read by core 1 in core1_main() below.
 *
 * The dispatch exists so X3's counter stays reachable at runtime. It is the
 * only state of this path ever proven on silicon, and a bring-up whose
 * fallback is `git revert` is a bring-up done blind -- which is exactly the
 * position X3 was in when it wedged the board twice. `smpstart` still runs
 * the counter; `smpstart join` is the new thing. */
volatile uint32_t g_core1_mode = CORE1_MODE_PROBE;

/* Core 1's entire program. No printk (the console path blocks and would
 * reach core 0's pinned uart task), no scheduler (core 1 has no task yet),
 * no interrupts (mstatus is zeroed in core1_entry). Just proof of life. */
static void core1_probe_main(void) {
    for (;;) {
        g_core1_ticks++;
    }
}

/* How far core 1 got, written into the same word core1_entry's arrival
 * marker uses (linker/rp2350.ld's .smpmark, which no boot path clears).
 *
 * X7's first hardware attempt died somewhere between core 1's first printk
 * and the scheduler, and took core 0 with it -- so there was no console left
 * to ask. A marker that survives is the only witness in that situation, and
 * it is three instructions. The step numbering matches the plan's own
 * ordering, which is what makes "it stopped at 05" an answer rather than a
 * starting point. */
void smp_mark(uint32_t step) {
    extern uint32_t _smp_mark;
    *(volatile uint32_t *)&_smp_mark = 0x51C0DE00u | step;
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

/* The same, for the primary. Both cores stopping is two questions, and the
 * marker only ever answered one of them. */
static void smp_mark0(uint32_t step) {
    extern uint32_t _smp_mark0;
    *(volatile uint32_t *)&_smp_mark0 = 0xC0DE0000u | step;
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

/* --- Cross-core mutual exclusion, measured -------------------------------
 *
 * §6.3 of the plan left this open and X7's own verify list named it: S1
 * proved `amoswap.w.aq`/`.rl` *executes* correctly on Hazard3, and X1 proved
 * the scheduler design on QEMU's two harts. Neither proved that the
 * instruction excludes one RP2350 core from another, which is a different
 * claim about the interconnect rather than about the core.
 *
 * Shaped like X3 deliberately: no scheduler, no traps, no printk on core 1 --
 * just two cores, one word, and a count that is either exact or is not. A
 * test that needs the scheduler cannot answer a question the scheduler
 * depends on.
 *
 * Bounded acquires rather than spin_lock_irqsave(): if the primitive really
 * is broken, an unbounded spin wedges the board and destroys the evidence.
 * A failed acquire is counted and reported.
 */
#define XLOCK_ITERS 50000u

volatile uint32_t g_xlock_word;
volatile uint32_t g_xlock_locked;
volatile uint32_t g_xlock_unlocked;
volatile uint32_t g_xlock_harts;
volatile uint32_t g_xlock_fail_c0;
volatile uint32_t g_xlock_fail_c1;
volatile uint32_t g_xlock_c1_done;
volatile uint32_t g_xlock_c1_ready;
volatile uint32_t g_xlock_go;
/* When each core's loop ran, so the overlap is measured rather than inferred
 * from whether the racy counter happened to lose anything. */
volatile uint64_t g_xlock_t0_start, g_xlock_t0_end;
volatile uint64_t g_xlock_t1_start, g_xlock_t1_end;

static bool xlock_take(uint32_t budget) {
    while (budget--) {
        if (arch_lock_try_acquire(&g_xlock_word)) return true;
    }
    return false;
}

static void xlock_run(unsigned hart, volatile uint32_t *fails) {
    for (uint32_t i = 0; i < XLOCK_ITERS; i++) {
        if (!xlock_take(200000u)) { (*fails)++; break; }
        g_xlock_locked++;
        g_xlock_harts |= 1u << hart;
        arch_lock_release(&g_xlock_word);

        /* Unlocked, deliberately: split read-modify-write widens the window,
         * so its losses are the contrast that says the exactness above meant
         * something. Same argument as smp_selftest()'s. */
        uint32_t v = g_xlock_unlocked;
        g_xlock_unlocked = v + 1;
    }
}

static void core1_locktest_main(void) {
    /* Start together, or this measures nothing.
     *
     * The first run on hardware reported a perfect locked count and `lost 0`
     * on the unlocked counter -- which is not a pass, it is the signature of
     * two loops that never overlapped. Core 1 has a bootrom launch and its
     * own entry path to get through while core 0 starts immediately, and
     * 50,000 iterations of a few instructions are gone in about a
     * millisecond. Both cores finished; neither met the other.
     *
     * smp_selftest() already documents this trap for the QEMU case ("if both
     * counters come out exact, there was no contention and the result proved
     * nothing"). Here it is the whole experiment, so the barrier is not
     * optional. */
    g_xlock_c1_ready = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    while (!g_xlock_go) { }

    g_xlock_t1_start = time_get_us();
    xlock_run(1, &g_xlock_fail_c1);
    g_xlock_t1_end = time_get_us();
    g_xlock_c1_done = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (;;) { g_core1_ticks++; }   /* fall back to proof-of-life */
}

/* Called from boot_header.S's core1_entry, on core 1, with tp/mtvec/stack
 * already set and interrupts off. Never returns. */
void core1_main(void) {
    smp_mark(3);
    if (g_core1_mode == CORE1_MODE_JOIN || g_core1_mode == CORE1_MODE_JOINQ) {
        secondary_main();       /* X7: trap_init, ticker, scheduler. */
    }
    if (g_core1_mode == CORE1_MODE_LOCKTEST) {
        core1_locktest_main();  /* §6.3. */
    }

    /* Staged bring-up: do the first N of secondary_main()'s steps, then fall
     * through to the counter. Core 0 can then see both how far the marker got
     * and whether core 1 is still executing at all -- which "it stopped at
     * step 7" alone cannot tell you. */
    if (g_core1_mode >= CORE1_MODE_STAGE1 && g_core1_mode <= CORE1_MODE_STAGE3) {
        trap_init();
        smp_mark(6);
    }
    if (g_core1_mode >= CORE1_MODE_STAGE2 && g_core1_mode <= CORE1_MODE_STAGE3) {
        ticker_arm_this_hart();
        smp_mark(7);
    }
    if (g_core1_mode == CORE1_MODE_STAGE3) {
        int pid = sched_secondary_init();
        smp_mark(8);
        if (pid >= 0) {
            uintptr_t f = spin_lock_irqsave(&g_smp_lock);
            g_online++;
            spin_unlock_irqrestore(&g_smp_lock, f);
            g_secondary_pid = pid;
        }
        smp_mark(9);
    }

    core1_probe_main();         /* X3: proof of life, and nothing else. */
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

    /* Leave core 1's mailbox empty.
     *
     * The SDK drains the FIFO around this handshake in both directions --
     * multicore_launch_core1_raw() disables the FIFO IRQ across it and
     * restores it afterwards, and multicore_reset_core1() has core 1 "drain
     * its own mailbox FIFO" before reporting ready. Anything left in the
     * FIFO keeps the SIO FIFO IRQ asserted, which is a live interrupt source
     * on a core that is about to enable interrupts. Cheap insurance next to
     * the cost of finding out. */
    fifo_drain();
    return i == 6;
}
/* --- X7 step 6: parking core 1 across a flash write ----------------------
 *
 * drivers/flash_rp2350.c turns XIP off, and for that window every
 * instruction fetch from 0x10000000-and-up faults or hangs. Its own routine
 * is `.ramfunc` and masks interrupts -- which protects the core executing
 * it, and says nothing about the other one. With core 1 scheduling, core 1
 * is running flash-resident code at that instant, and the failure is a hung
 * board with nothing left to report it.
 *
 * So core 0 asks, and waits for an acknowledgement from code that is
 * demonstrably no longer in flash. The handshake is deliberately in that
 * direction: "core 1 told me it is parked in RAM" is checkable, whereas
 * "core 1 should be idle by now" is a hope.
 *
 * Both words live in .bss and the spin itself is `.ramfunc`, so nothing on
 * core 1's side of the barrier touches the XIP window.
 */
volatile uint32_t g_flash_park_req;    /* core 0 -> core 1: park now */
volatile uint32_t g_flash_park_ack;    /* core 1 -> core 0: parked, in RAM */

/* The parking loop itself. Runs on core 1, from RAM, with interrupts already
 * masked by its caller -- a timer tick here would vector to a handler in
 * flash, which is the whole thing being avoided. */
__attribute__((section(".ramfunc"), noinline))
static void core1_park_spin(void) {
    g_flash_park_ack = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    while (g_flash_park_req) { }
    g_flash_park_ack = 0;
}

/* Called from sched_yield() on every hart, and cheap by construction: one
 * load of a .bss word that is zero except during a flash write. Only core 1
 * parks -- core 0 is the one doing the writing. */
void smp_flash_park_check(void) {
    if (!g_flash_park_req || hart_id() == 0) return;
    uintptr_t f = irq_save();
    core1_park_spin();
    irq_restore(f);
}

/* Called by core 0 immediately before XIP goes down. Returns true if core 1
 * is parked in RAM *or* was never scheduling in the first place; false if it
 * is running and did not park in time.
 *
 * A timeout refuses the flash write rather than proceeding hopefully. That
 * is the one decision here that matters: a refused write is an error the
 * caller can report, and a write that proceeds anyway is a board that stops
 * mid-erase. */
bool smp_flash_park_request(void) {
    if (smp_harts_online() < 2) return true;   /* nothing to park */

    g_flash_park_req = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* Generous: core 1 reaches the check on its next sched_yield(), and the
     * preemption tick guarantees one within a tick even if it is running a
     * task that never yields on its own. */
    uint64_t deadline = time_get_us() + 200000;   /* 200 ms */
    while (!g_flash_park_ack && time_get_us() < deadline) { }

    if (g_flash_park_ack) return true;

    g_flash_park_req = 0;
    return false;
}

void smp_flash_park_release(void) {
    g_flash_park_req = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

int smp_start_secondary(unsigned mode) {
    /* One launch per boot, whatever the mode. harts_online() alone is not
     * enough: probe and locktest leave core 1 running without ever entering
     * the scheduler, so it stays 1 while core 1 is very much alive, and a
     * second handshake would be talking to a core that is not listening. */
    static bool launched;
    if (launched || smp_harts_online() > 1) {
        printk("[SMP] core 1 has already been launched this boot; reboot first\n");
        return -1;
    }
    launched = true;

    smp_mark0(1);

    /* The deadman goes on FIRST, before anything that can hang.
     *
     * It used to be armed after the launch handshake, which was too late in
     * two ways. The handshake itself can wedge, and -- less obviously -- the
     * console on this board is USB, whose ring is drained by a task: a core 0
     * that dies shortly after printing loses the message it printed, so
     * "there was no output" never meant "it died before printing". Two runs
     * were read that way before the distinction became clear. Arming here
     * means the board comes back and .smpmark says how far core 1 got,
     * whatever happened to the console. */
    if (mode >= CORE1_MODE_JOIN) {
        if (!rp2350_reboot_after_ms(8000)) {
            printk("[SMP] warning: could not arm the reboot deadman; "
                   "a hang here needs a physical BOOTSEL\n");
        } else {
            printk("[SMP] deadman armed: the board reboots in 8 s unless this finishes\n");
        }
    }

    smp_mark0(2);
    uint32_t before = g_core1_ticks;

    /* Published before the handshake, and with a release fence: core 1 reads
     * this as its very first C statement, so the store must be visible by
     * then. The FIFO writes below are ordered after it on this core, but
     * that is not by itself an ordering guarantee for the other one. */
    g_core1_mode = mode;
    smp_paint_core1_stack();
    smp_mark0(3);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    printk("[SMP] launching core 1 in %s mode (ticks before: %lu)\n",
           mode == CORE1_MODE_JOIN     ? "JOIN"     :
           mode == CORE1_MODE_LOCKTEST ? "locktest" : "probe",
           (unsigned long)before);

    if (!smp_launch_core1()) {
        rp2350_reboot_cancel();
        printk("[SMP] core 1 did not answer the launch handshake (still parked)\n");
        return -1;
    }
    smp_mark0(4);
    printk("[SMP] launch handshake completed\n");
    smp_mark0(5);

    if (mode == CORE1_MODE_LOCKTEST) {
        /* Core 0's half runs here, concurrently with core 1's. Interrupts
         * masked across each critical section only -- a preemption while
         * holding the word would be harmless (nothing else on this core
         * touches it) but would skew the contention this is measuring. */
        /* Wait for core 1 to reach its side of the barrier, then release
         * both. Bounded: a core 1 that never arrives is reported, not spun
         * on. */
        uint64_t ready_by = time_get_us() + 1000000;   /* 1 s */
        while (!g_xlock_c1_ready && time_get_us() < ready_by) { }
        if (!g_xlock_c1_ready) {
            printk("[SMP] XLOCK_FAIL -- core 1 never reached the barrier\n");
            return -1;
        }
        g_xlock_go = 1;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        uintptr_t f = irq_save();
        g_xlock_t0_start = time_get_us();
        xlock_run(0, &g_xlock_fail_c0);
        g_xlock_t0_end = time_get_us();
        irq_restore(f);

        uint64_t deadline = time_get_us() + 2000000;   /* 2 s */
        while (!g_xlock_c1_done && time_get_us() < deadline) { }

        uint32_t want = 2u * XLOCK_ITERS;
        printk("[SMP] cross-core lock: locked=%lu (want %lu) unlocked=%lu (lost %lu)\n",
               (unsigned long)g_xlock_locked, (unsigned long)want,
               (unsigned long)g_xlock_unlocked,
               (unsigned long)(want - g_xlock_unlocked));
        printk("[SMP] cores seen=0x%lx  acquire failures: core0=%lu core1=%lu  core1 finished: %s\n",
               (unsigned long)g_xlock_harts,
               (unsigned long)g_xlock_fail_c0, (unsigned long)g_xlock_fail_c1,
               g_xlock_c1_done ? "yes" : "NO");

        /* The overlap, in microseconds. Two loops that both finished prove
         * nothing about exclusion if they never ran at the same time, and
         * the first hardware run looked perfect for exactly that reason. */
        uint64_t lo = g_xlock_t0_start > g_xlock_t1_start ? g_xlock_t0_start : g_xlock_t1_start;
        uint64_t hi = g_xlock_t0_end   < g_xlock_t1_end   ? g_xlock_t0_end   : g_xlock_t1_end;
        long overlap = (hi > lo) ? (long)(hi - lo) : 0;
        printk("[SMP] core0 ran %lu..%lu us, core1 ran %lu..%lu us -- overlap %ld us\n",
               (unsigned long)g_xlock_t0_start, (unsigned long)g_xlock_t0_end,
               (unsigned long)g_xlock_t1_start, (unsigned long)g_xlock_t1_end,
               overlap);

        bool ok = (overlap > 0) && (g_xlock_locked == want) && (g_xlock_harts == 0x3u) &&
                  g_xlock_c1_done && !g_xlock_fail_c0 && !g_xlock_fail_c1;

        /* Reported, never asserted: "a race must lose at least one update" is
         * probabilistic, and a test that fails when the machine happens to
         * schedule kindly is worse than one that says what it saw. But with
         * the barrier above, no loss at all means the two loops still did not
         * meet -- and then the exact count above is not evidence of anything.
         * Saying so is the difference between a result and a green tick. */
        if (g_xlock_unlocked == want) {
            printk("[SMP] note: the unlocked counter lost nothing. With a real overlap\n"
                   "      above that is luck; with none, the exact count proves nothing\n");
        }
        if (overlap <= 0) {
            printk("[SMP] the two loops never ran at the same time -- this measured nothing\n");
        }

        printk(ok ? "[SMP] XLOCK_OK -- amoswap excludes one Hazard3 core from the other\n"
                  : "[SMP] XLOCK_FAIL -- cross-core mutual exclusion did NOT hold\n");
        return ok ? 0 : -1;
    }

    if (mode >= CORE1_MODE_JOIN) {
        /* Core 1 announces itself from secondary_main(); what this waits for
         * is the scheduler count, which only code running there can raise.
         * Bounded on the microsecond timer, and a timeout is reported rather
         * than spun on: a core that launched but never reached the scheduler
         * is a different failure from one that never launched, and the two
         * must not look the same. */
        /* Watch core 1's progress rather than only its result.
         *
         * Between its first printk and going online, core 1 emits nothing --
         * it only writes .smpmark. So core 0 can safely report what it sees
         * without contending for the console with a core that is mid-bring-up,
         * and a step that is reached but never left names itself. This is the
         * console X3 did not have. */
        /* Record silently, report afterwards.
         *
         * The first version of this printk()'d each step as it saw it, and
         * that put core 0 into the console -- printk, uart batch, a blocking
         * chan_call to the uart task -- for the whole of core 1's bring-up
         * window. Core 1 then stalled at a different step on each run (0x09,
         * then 0x07), which is the signature of contention rather than of a
         * broken step. An observer that changes what it observes is not an
         * observer.
         *
         * So the wait touches nothing but two volatile loads and a timer:
         * no console, no locks beyond the one harts_online() needs. The
         * trace is printed once core 1 is either up or gone. */
        extern uint32_t _smp_mark;
        uint32_t trace[16];
        unsigned n = 0;
        uint32_t last = 0;
        uint64_t deadline = time_get_us() + 500000;   /* 500 ms */
        smp_mark0(6);
        while (smp_harts_online() < 2 && time_get_us() < deadline) {
            uint32_t m = *(volatile uint32_t *)&_smp_mark;
            if (m != last) {
                last = m;
                if (n < 16) trace[n++] = m;
            }
        }

        smp_mark0(7);

        /* The quiet variant answers the question the noisy one cannot.
         *
         * core0_probe read back 0xc0de0007 -- core 0 got past the wait, and
         * died in the printk immediately after, while core 1 was idling in
         * the scheduler. So "does core 1 in the scheduler kill the board" and
         * "does the console survive two cores" are different questions, and
         * every run so far has answered them together. This one uses no
         * console at all: it marks, waits two seconds alongside a live core
         * 1, marks again, and reboots on purpose. If the second mark is
         * there, core 1 is not the problem. */
        if (mode == CORE1_MODE_JOINQ) {
            smp_mark0(0x20);
            uint64_t until = time_get_us() + 2000000;
            while (time_get_us() < until) { }
            smp_mark0(0x21);
            if (smp_harts_online() >= 2) smp_mark0(0x22);
            rp2350_reboot_cancel();
            rp2350_reboot();          /* deliberate, so the marks are readable */
            for (;;) { }
        }

        printk("[SMP] core 1 step trace:");
        for (unsigned i = 0; i < n; i++) printk(" 0x%lx", (unsigned long)trace[i]);
        printk("%s\n", n ? "" : " (none seen)");

        if (mode != CORE1_MODE_JOIN && mode != CORE1_MODE_JOINQ) {
            /* A stage stops short of the scheduler on purpose, so
             * harts_online never reaches 2. What says it survived is the
             * counter: core 1 falls into core1_probe_main() after its last
             * stage, so a moving counter means every step it did perform
             * left it alive and executing. */
            rp2350_reboot_cancel();
            uint32_t a = g_core1_ticks;
            uint64_t until = time_get_us() + 50000;
            while (time_get_us() < until) { }
            uint32_t b = g_core1_ticks;
            printk("[SMP] stage %lu: marker 0x%lx, counter %lu -> %lu (delta %lu)\n",
                   (unsigned long)(mode - CORE1_MODE_STAGE1 + 1),
                   (unsigned long)*(volatile uint32_t *)&_smp_mark,
                   (unsigned long)a, (unsigned long)b, (unsigned long)(b - a));
            printk(b != a ? "[SMP] STAGE_OK -- core 1 survived every step of this stage\n"
                          : "[SMP] STAGE_DEAD -- core 1 stopped executing\n");
            return (b != a) ? 0 : -1;
        }
        smp_mark0(8);
        if (smp_harts_online() >= 2) {
            rp2350_reboot_cancel();
            smp_mark0(9);
            printk("[SMP] CORE1_SCHEDULING -- core 1 joined the scheduler as pid %d\n",
                   g_secondary_pid);
            printk("[SMP] core 1 stack: %lu of %lu bytes used\n",
                   (unsigned long)smp_core1_stack_used(),
                   (unsigned long)((uintptr_t)&_stack1_top -
                                   (uintptr_t)&_stack1_bottom));
            return 0;
        }
        rp2350_reboot_cancel();
        printk("[SMP] CORE1_STALLED -- launched, but never reached the scheduler\n");
        printk("[SMP] last step core 1 reached: 0x%lx "
               "(03 core1_main, 04 vmm, 05 first printk, 06 trap_init, "
               "07 ticker, 08 sched_secondary_init, 09 online, 10 idle)\n",
               (unsigned long)*(volatile uint32_t *)&_smp_mark);
        return -1;
    }

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

#if !(CONFIG_ENABLE_SMP && defined(CONFIG_BOARD_RP2350))
/* Every other target: there is no second core to park, and no XIP window to
 * lose. Stubs rather than #ifdefs at the call sites, so sched_yield() and
 * the flash driver read the same on every board. */
bool smp_flash_park_request(void) { return true; }
void smp_flash_park_release(void) { }
void smp_flash_park_check(void) { }
#endif

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
    vmm_secondary_init();
    smp_mark(4);

    /* Say we are here, from the window where this hart owns no task.
     *
     * This line is the point of the identity fix, not decoration. Everything
     * between the reset vector and sched_secondary_init() below runs with no
     * task slot, and until that fix sched_current_pid() answered 0 here --
     * the boot task, running on a different hart at this instant. A printk
     * from this window could take the re-entrant path through a lock hart 0
     * was holding, and its task_block() on contention would have blocked the
     * shell. So bring-up was undebuggable exactly where debugging matters:
     * X3 got its second core running only by giving it no printk at all.
     *
     * The pid is printed rather than assumed. -1 is the fix working; 0 is the
     * bug, and the two are distinguishable in the boot log without having to
     * reproduce the race. */
    printk("[SMP] hart %u: in the kernel, no task yet (pid %d)\n",
           hart_id(), sched_current_pid());
    smp_mark(5);

    /* Per-hart interrupt state. trap_init() writes this hart's own PLIC
     * context since §6.1's fix; before it, this call would have
     * reconfigured hart 0's. */
    trap_init();
    smp_mark(6);

    /* This hart's own preemption deadline. stimecmp and sie are per-hart
     * CSRs, so arming is genuinely local -- what is NOT local is the tick
     * *rate*, which the primary measured once and this reuses. */
    ticker_arm_this_hart();
    smp_mark(7);

    /* Become a task, so there is something to switch away from. */
    int pid = sched_secondary_init();
    smp_mark(8);
    if (pid < 0) {
        printk("[SMP] hart %u: no free task slot; parking\n", hart_id());
        for (;;) { __asm__ __volatile__("wfi"); }
    }

    {
        uintptr_t f = spin_lock_irqsave(&g_smp_lock);
        g_online++;
        spin_unlock_irqrestore(&g_smp_lock, f);
    }
    /* Core 0 reports this, not core 1.
     *
     * The first attempt printk()'d here and died doing it -- .smpmark read
     * back 0x51c0de09, i.e. past sched_secondary_init() and past g_online++,
     * stopped inside this line. It is the first printk core 1 makes while
     * *owning a task*, which is the first one that takes the blocking
     * chan_call() to the uart task pinned to core 0 rather than the
     * no-task fallback. Whether that path is safe during bring-up is a
     * question for after core 1 is stably scheduling, not a thing to answer
     * by wedging the board.
     *
     * So the bring-up core says nothing and the healthy core reports on its
     * behalf -- X3's discipline, which is what got a second core running at
     * all. g_secondary_pid is published for core 0 to read. */
    g_secondary_pid = pid;
    smp_mark(10);

    /* Interrupts on, then idle: pull whatever the shared ready queue has,
     * and halt between attempts.
     *
     * X1 wrote this as a bare `for(;;) sched_yield();` and said why: a wfi
     * would need a cross-hart wakeup on every task_unblock(), which is real
     * work, and the cost of spinning was "a busy core on an idle system,
     * which on QEMU is a host thread and on RP2350 would be power". It also
     * said "X2 or later can make it wfi once there is a reason to".
     *
     * X7 is that reason, and the cost was not power. sched_yield() takes
     * g_sched_lock on every call, so a bare spin here hammers that lock from
     * core 1 at full speed -- and core 0 needs the same lock for every
     * context switch, every task_block() and every task_unblock(). On QEMU
     * the two harts are host threads and the OS shares the CPU between them,
     * so the starvation never appears. On silicon they share one bus, core 0
     * loses, and the machine stops: this is what killed the board on X7's
     * join attempts while the staged runs -- whose tail is a plain counter
     * loop that takes no locks -- survived every time.
     *
     * The first fix was `wfi` between yields, waking on this hart's own timer
     * so no cross-hart IPI was needed. It works and it is too blunt: QEMU's
     * X2 check immediately reported domains_hart1: 0, because a hart that
     * sleeps until the next tick never picks up a short-lived task at all.
     * Fixing starvation by making the second hart useless is not a fix.
     *
     * So the idle hart asks a lock-free question instead, and only pays for
     * the lock when the answer is yes. sched_peek_runnable() reads plain
     * words; the backoff below touches no shared memory whatsoever, so an
     * idle secondary generates no bus traffic the primary has to contend
     * with. Latency stays in microseconds rather than up to a tick. */
    irq_restore(IRQ_ENABLE_BIT);
    for (;;) {
        if (sched_peek_runnable()) {
            sched_yield();
            continue;
        }
        /* Nothing for us. Back off on a local, so the primary owns the bus
         * while this hart has nothing to do with it. */
        for (volatile int spin = 0; spin < 64; spin++) { }
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

/* Without a second hart there is nothing to distribute across, so this says
 * so rather than starting a load that could only ever report one hart --
 * the same reasoning as smp_selftest()'s skip above. */
int smp_load_cmd(const char *arg) {
    (void)arg;
    cprintf("SMP load:\n");
    cprintf("  [skip] this build has CONFIG_ENABLE_SMP=0; there is no second hart\n");
    cprintf("         (cmake --preset rv64-smp, then run under qemu -smp 2)\n");
    cprintf("SMPLOAD_SKIPPED\n");
    return 0;
}

void smp_load_note_fault(void) { }

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


/* --- X5's background load ------------------------------------------------
 *
 * The milestone asks for the existing isolation and fault suite re-run "with
 * tasks actually distributed across both cores, not just possible in
 * principle". The distinction is the whole point, and it is easy to lose:
 * every one of those tests already passes on a two-hart kernel, because the
 * driver tasks are pinned to hart 0 (X2) and a short-lived probe task
 * started from the shell will usually be picked up by the hart the shell is
 * on. Such a run proves nothing that the single-hart run did not.
 *
 * So this provides the missing half: a background load that keeps every hart
 * holding a real task, and -- more importantly -- evidence about the
 * *instant* the fault was taken. smp_load_note_fault() snapshots every
 * hart's progress from inside the trap handler, before the faulting task is
 * killed. If the other hart's counter was already moving before that
 * snapshot and kept moving after it, the other hart was executing a task at
 * the moment the fault landed. That is a claim about simultaneity, which no
 * amount of "both tests passed" can establish.
 *
 * The loaders yield rather than spin: a busy-wait load would test the
 * scheduler's ability to be starved, which is not what is being asked.
 */

#define SMPLOAD_TASKS (MAX_HARTS * 2)

static spinlock_t        g_load_lock;
static volatile uint32_t g_load_prog[MAX_HARTS];      /* work done, per hart */
static volatile int      g_load_run;                  /* loaders exit at 0 */
static volatile int      g_load_alive;
static volatile uint32_t g_load_at_fault[MAX_HARTS];  /* the snapshot */
static volatile unsigned g_load_fault_hart;
static volatile uint32_t g_load_faults;

void smp_load_note_fault(void) {
    if (!g_load_run) return;   /* no load: nothing to say, and nothing to lock */

    uintptr_t f = spin_lock_irqsave(&g_load_lock);
    for (unsigned h = 0; h < MAX_HARTS; h++) g_load_at_fault[h] = g_load_prog[h];
    g_load_fault_hart = hart_id();
    g_load_faults++;
    spin_unlock_irqrestore(&g_load_lock, f);
}

/* Interrupts are off across the increment, which is why hart_id() and the
 * slot it selects cannot disagree: a task can only migrate at a switch, and
 * no switch can happen here. The lock is what makes the counter exact
 * between the several loaders sharing one hart. */
static void smpload_worker(void *arg) {
    (void)arg;
    for (;;) {
        uintptr_t f = spin_lock_irqsave(&g_load_lock);
        int run = g_load_run;
        if (run) g_load_prog[hart_id()]++;
        spin_unlock_irqrestore(&g_load_lock, f);
        if (!run) break;
        sched_yield();
    }
    uintptr_t f = spin_lock_irqsave(&g_load_lock);
    g_load_alive--;
    spin_unlock_irqrestore(&g_load_lock, f);
}

static void smpload_report(const char *tag) {
    uintptr_t f = spin_lock_irqsave(&g_load_lock);
    uint32_t p[MAX_HARTS];
    for (unsigned h = 0; h < MAX_HARTS; h++) p[h] = g_load_prog[h];
    uint32_t faults = g_load_faults;
    int alive = g_load_alive;
    spin_unlock_irqrestore(&g_load_lock, f);

    cprintf("%s alive=%d faults=%u", tag, alive, (unsigned)faults);
    for (unsigned h = 0; h < MAX_HARTS; h++) cprintf(" hart%u=%u", h, (unsigned)p[h]);
    cprintf("\n");
}

static int smpload_start(void) {
    if (g_load_run) {
        cprintf("SMPLOAD_ALREADY_RUNNING\n");
        return 0;
    }
    spinlock_init(&g_load_lock);
    for (unsigned h = 0; h < MAX_HARTS; h++) { g_load_prog[h] = 0; g_load_at_fault[h] = 0; }
    g_load_faults = 0;
    g_load_fault_hart = (unsigned)-1;
    g_load_alive = 0;
    g_load_run = 1;

    int made = 0;
    for (int i = 0; i < SMPLOAD_TASKS; i++) {
        if (task_create("smpload", smpload_worker, NULL) >= 0) { made++; g_load_alive++; }
    }
    if (made == 0) {
        g_load_run = 0;
        cprintf("SMPLOAD_FAIL (no free task slot)\n");
        return 1;
    }

    /* Let them actually reach a hart before returning, so a test that reads
     * the counters immediately does not see a zero it would have to retry
     * around. Bounded, because a load that cannot start must not hang the
     * shell it was typed into. */
    for (int guard = 0; guard < 200000; guard++) {
        uintptr_t f = spin_lock_irqsave(&g_load_lock);
        uint32_t p0 = g_load_prog[0];
        spin_unlock_irqrestore(&g_load_lock, f);
        if (p0 > 0) break;
        sched_yield();
    }

    cprintf("SMPLOAD_STARTED tasks=%d\n", made);
    return 0;
}

static int smpload_stop(void) {
    if (!g_load_run) {
        cprintf("SMPLOAD_NOT_RUNNING\n");
        return 0;
    }

    /* Read the evidence *before* stopping: after the loaders exit, "the
     * other hart kept going after the fault" would be indistinguishable
     * from "the other hart kept going until stop was typed". */
    uintptr_t f = spin_lock_irqsave(&g_load_lock);
    uint32_t p[MAX_HARTS], at[MAX_HARTS];
    for (unsigned h = 0; h < MAX_HARTS; h++) { p[h] = g_load_prog[h]; at[h] = g_load_at_fault[h]; }
    uint32_t faults = g_load_faults;
    unsigned fh = g_load_fault_hart;
    g_load_run = 0;
    spin_unlock_irqrestore(&g_load_lock, f);

    for (int guard = 0; guard < 2000000 && g_load_alive > 0; guard++) sched_yield();

    int fail = 0;
    cprintf("SMP load, across the isolation and fault suite:\n");
    if (faults) cprintf("  faults taken while loaded: %u (last handled by hart %u)\n",
                        (unsigned)faults, fh);
    else        cprintf("  faults taken while loaded: none\n");
    for (unsigned h = 0; h < MAX_HARTS; h++) {
        if (faults) cprintf("  hart %u: progress %u, of which %u before the last fault\n",
                            h, (unsigned)p[h], (unsigned)at[h]);
        else        cprintf("  hart %u: progress %u\n", h, (unsigned)p[h]);
    }

    /* Not "no fault happened to occur": a load started and stopped with no
     * isolation test run in between has established nothing, and saying so
     * is the point. The alternative -- passing quietly -- is how a suite
     * ends up reporting that it verified something it never ran. */
    bool any_fault = (faults > 0);
    cprintf("  [%s] a task was actually killed while the load was running\n",
            any_fault ? "ok" : "FAIL");
    if (!any_fault) fail++;

    /* The claim the milestone rides on: at the instant of the fault, a hart
     * other than the one taking it was mid-task -- it had made progress
     * before the snapshot and made more after it. Both halves are needed.
     * "Progress before" alone would pass for a hart that had since gone
     * idle; "progress after" alone, for one that only started afterwards. */
    bool concurrent = false;
    for (unsigned h = 0; h < MAX_HARTS; h++) {
        if (h == fh) continue;
        if (at[h] > 0 && p[h] > at[h]) concurrent = true;
    }
    cprintf("  [%s] another hart was mid-task at the instant of the fault\n",
            concurrent ? "ok" : "FAIL");
    if (!concurrent) fail++;

    bool drained = (g_load_alive == 0);
    cprintf("  [%s] every loader exited when asked\n", drained ? "ok" : "FAIL");
    if (!drained) fail++;

    if (fail == 0) cprintf("SMPLOAD_OK (3/3)\n");
    else           cprintf("SMPLOAD_FAIL (%d failed)\n", fail);
    return fail;
}

int smp_load_cmd(const char *arg) {
    if (arg && arg[0] == 's' && arg[1] == 't' && arg[2] == 'a') return smpload_start();
    if (arg && arg[0] == 's' && arg[1] == 't' && arg[2] == 'o') return smpload_stop();
    smpload_report("SMPLOAD");
    return 0;
}

#endif /* CONFIG_ENABLE_SMP */
