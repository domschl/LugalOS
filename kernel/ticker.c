#include "kernel/ticker.h"
#include "kernel/hart.h"
#include "kernel/printk.h"
#include "arch/csr.h"
#include "arch/trap.h"
#include "kernel/time.h"

/* See kernel/include/kernel/ticker.h for why this has three backends. */

static bool     g_enabled;
static uint32_t g_hz;
static uint64_t g_measured_hz;   /* 0 = the nominal TICK_HZ was used */
/* Per-hart, not global (X1/X4, plan/phase23_multicore_scheduling.md).
 *
 * Every reader asks the same question -- "how many times was *I* preempted"
 * -- and none of them wants a system-wide total: kernel/shell.c's priostress
 * and preemption measurements, and kernel/lock.c's check that an irqsave-held
 * spinlock really masks interrupts.
 *
 * That last one is what forced the decision. It holds a spinlock with
 * interrupts off on this hart and asserts the tick counter is frozen; with a
 * single global counter the *other* hart's timer keeps incrementing it, so
 * the check failed on a kernel whose masking was perfectly correct. S5
 * (plan/phase22 §S5) recorded g_ticks as racy-on-two-harts and handed the
 * choice between "one atomic counter" and "one per hart" to X4. X1 answered
 * it by needing the answer: per hart, because that is what the question
 * means. A global count of ticks across all harts is a number nothing in
 * this tree has ever wanted. */
static uint64_t g_ticks[MAX_HARTS];
static uint64_t g_interval;   /* in whatever unit the target's clock counts */

bool     ticker_enabled(void) { return g_enabled; }

uint64_t ticker_ticks(void)   { return g_ticks[hart_id()]; }
void     ticker_count_tick(void) { g_ticks[hart_id()]++; }

/* --- RP2350: SIO mtime/mtimecmp -------------------------------------- */
#if defined(CONFIG_BOARD_RP2350)

#define SIO_BASE        0xd0000000UL
#define SIO_MTIME_CTRL  (*(volatile uint32_t *)(SIO_BASE + 0x1a4))
#define SIO_MTIME       (*(volatile uint32_t *)(SIO_BASE + 0x1b0))
#define SIO_MTIMEH      (*(volatile uint32_t *)(SIO_BASE + 0x1b4))
#define SIO_MTIMECMP    (*(volatile uint32_t *)(SIO_BASE + 0x1b8))
#define SIO_MTIMECMPH   (*(volatile uint32_t *)(SIO_BASE + 0x1bc))
#define MTIME_CTRL_EN   (1u << 0)

#define TICKS_BASE        0x40108000UL
#define TICKS_RISCV_CTRL   (*(volatile uint32_t *)(TICKS_BASE + 0x3c))
#define TICKS_RISCV_CYCLES (*(volatile uint32_t *)(TICKS_BASE + 0x40))
#define TICKS_CTRL_ENABLE  (1u << 0)
#define TICKS_CTRL_RUNNING (1u << 1)

/* Nominal only -- the real rate is measured at init, see below. */
#define TICK_HZ 1000000UL

static uint64_t now(void) {
    uint32_t hi, lo;
    do { hi = SIO_MTIMEH; lo = SIO_MTIME; } while (hi != SIO_MTIMEH);
    return ((uint64_t)hi << 32) | lo;
}

static void set_deadline(uint64_t t) {
    /* Write the high half to all-ones first so the 64-bit comparator can
     * never momentarily hold a value in the past while half-updated, which
     * would fire a spurious interrupt. */
    SIO_MTIMECMPH = 0xffffffffu;
    SIO_MTIMECMP  = (uint32_t)t;
    SIO_MTIMECMPH = (uint32_t)(t >> 32);
}

static bool arch_ticker_init(void) {
    /* mtime does not run on its own. It is driven by a tick generator in the
     * TICKS block whose ENABLE bit resets to 0, so without this the counter
     * sits at zero forever and no deadline is ever reached. That was half of
     * why the first attempt at RP2350 preemption failed; the other half was a
     * 4 KB boot stack in SCRATCH_Y, which a trap frame taken deep inside the
     * Lisp evaluator could overflow into the heap. The stack now lives in RAM
     * (see linker/rp2350.ld).
     *
     * CYCLES is in source-clock cycles per tick. 12 assumes a 12 MHz clk_ref,
     * which is the usual XOSC configuration -- but rather than trust that, the
     * rate is measured below against the microsecond timer that time.c already
     * uses and is known good. */
    TICKS_RISCV_CTRL = 0;
    TICKS_RISCV_CYCLES = 12;
    TICKS_RISCV_CTRL = TICKS_CTRL_ENABLE;

    /* RUNNING is a status bit synchronised to the source clock, so it is not
     * asserted the instant ENABLE is written -- checking it immediately reads
     * back 0 and looks like a hardware failure. Poll it briefly instead. */
    {
        uint64_t deadline_us = time_get_us() + 5000;
        while (!(TICKS_RISCV_CTRL & TICKS_CTRL_RUNNING)) {
            if (time_get_us() > deadline_us) {
                printk("[Ticker] RISC-V tick generator did not start; preemption off\n");
                return false;
            }
        }
    }

    /* Measure the real tick rate instead of assuming it. */
    uint64_t t0 = now();
    uint64_t us0 = time_get_us();
    while (time_get_us() - us0 < 2000) { /* 2 ms is plenty to divide by */ }
    uint64_t elapsed_ticks = now() - t0;
    uint64_t elapsed_us = time_get_us() - us0;

    if (elapsed_ticks == 0 || elapsed_us == 0) {
        printk("[Ticker] mtime is not advancing; preemption off\n");
        return false;
    }
    /* ticks per second, from the measurement */
    uint64_t measured_hz = (elapsed_ticks * 1000000UL) / elapsed_us;
    g_measured_hz = measured_hz;
    /* Measured, not assumed. This used to come out near 2.33 MHz rather than
     * the 1 MHz a 12 MHz clk_ref implies, and the note here blamed this
     * generator. It was the other way round: this one was right and the
     * reference it was measured against was slow, because the boot code
     * OR-ed 12 into a TIMER0 CYCLES register the bootrom had left non-zero
     * and got 28 cycles per tick (fixed 2026-08-23, arch/riscv/rp2350/
     * boot_header.S). It should now read close to 1 MHz. The absolute figure
     * is still only as good as time.c's microsecond timer; the ratio is what
     * preemption actually needs. */
    g_interval = measured_hz / g_hz;
    if (g_interval == 0) g_interval = 1;

    set_deadline(now() + g_interval);
    set_csr(mie, 1UL << 7);   /* MTIE */
    return true;
}

/* --- QEMU RV32: CLINT, M-mode ---------------------------------------- */
#elif !defined(CONFIG_MODE_S)

#define CLINT_BASE     0x02000000UL
#define CLINT_MTIMECMP (*(volatile uint64_t *)(CLINT_BASE + 0x4000))
#define CLINT_MTIME    (*(volatile uint64_t *)(CLINT_BASE + 0xBFF8))

/* QEMU's virt machine clocks the CLINT at 10 MHz. */
#define TICK_HZ 10000000UL

static uint64_t now(void) { return CLINT_MTIME; }

static void set_deadline(uint64_t t) { CLINT_MTIMECMP = t; }

static bool arch_ticker_init(void) {
    set_deadline(now() + g_interval);
    set_csr(mie, 1UL << 7);   /* MTIE */
    return true;
}

/* --- QEMU RV64: Sstc stimecmp, S-mode -------------------------------- */
#else

#define TICK_HZ 10000000UL    /* same virt machine clock, read via rdtime */

static uint64_t now(void) {
    uint64_t t;
    __asm__ __volatile__("rdtime %0" : "=r"(t));
    return t;
}

static void set_deadline(uint64_t t) {
    /* stimecmp is CSR 0x14d. Named numerically because the assembler in use
     * does not know the Sstc mnemonic. */
    __asm__ __volatile__("csrw 0x14d, %0" :: "r"(t));
}

static bool arch_ticker_init(void) {
    /* Sstc is only usable if M-mode set menvcfg.STCE before dropping to
     * S-mode -- entry.S does. Probe by writing a deadline and reading it
     * back: on a core without Sstc the CSR access traps, which
     * arch_probe_begin() turns into an answer rather than a halt. */
    arch_probe_begin();
    set_deadline(now() + g_interval);
    if (arch_probe_faulted()) {
        printk("[Ticker] Sstc (stimecmp) unavailable; preemption stays off\n");
        return false;
    }
    set_csr(sie, 1UL << 5);   /* STIE */
    return true;
}

#endif

void ticker_next(void) {
    if (!g_enabled) return;
    set_deadline(now() + g_interval);
}

/* Arms the calling hart's own preemption deadline (X1,
 * plan/phase23_multicore_scheduling.md).
 *
 * Split out of ticker_init() because those two do different kinds of work.
 * ticker_init() programs shared hardware and *measures* the tick rate -- on
 * RP2350 it enables a chip-wide tick generator and calibrates it against a
 * busy loop, which a second hart must never re-run (S5's disposition,
 * plan/phase22 §S5). What is genuinely per-hart is only the comparator and
 * the interrupt-enable bit, which is all this does, reusing the interval the
 * primary already established.
 *
 * X4 owns the RP2350 half of this: whether each Hazard3 core has its own
 * mtimecmp is a datasheet question, and until it is answered this is
 * QEMU-only. */
void ticker_arm_this_hart(void) {
    if (!g_enabled) return;
    set_deadline(now() + g_interval);
#if defined(CONFIG_MODE_S)
    set_csr(sie, 1UL << 5);   /* STIE */
#else
    set_csr(mie, 1UL << 7);   /* MTIE */
#endif
}

bool ticker_init(uint32_t hz) {
    if (hz == 0) return false;
    g_hz = hz;
    g_interval = TICK_HZ / hz;
    if (g_interval == 0) g_interval = 1;

    if (!arch_ticker_init()) {
        g_enabled = false;
        return false;
    }
    g_enabled = true;
    printk("[Ticker] Preemption timer at %u Hz (%lu ticks of a %lu Hz clock%s)\n",
           (unsigned)hz, (unsigned long)g_interval,
           (unsigned long)(g_measured_hz ? g_measured_hz : TICK_HZ),
           g_measured_hz ? ", measured" : "");
    return true;
}
