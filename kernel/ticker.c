#include "kernel/ticker.h"
#include "kernel/hart.h"
#include "kernel/printk.h"
#include "arch/csr.h"
#include "arch/trap.h"
#include "kernel/time.h"
#if defined(CONFIG_BOARD_ESP32P4)
#include "arch/esp32p4_intr.h"
#endif

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

/* 0 when no measurement was taken -- see the header. Not clamped to TICK_HZ
 * here: a caller wants to know *whether* this was measured, and folding the
 * nominal value in would take that away. */
uint64_t ticker_measured_hz(void) { return g_measured_hz; }

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

    /* Measure the real tick rate instead of assuming it.
     *
     * The same warm-up the ESP32-P4 arm below needs, and here it is
     * insurance rather than a fix: this board measures its 1 MHz source as
     * exactly 1 000 000 Hz, checked on hardware before and after adding
     * these two lines. It is already accidentally warm -- the RUNNING poll
     * above calls time_get_us() at least once -- and its `now()` is two SIO
     * register reads rather than the P4's systimer handshake. Keeping the
     * two arms the same shape means the next person to read one of them
     * does not have to work out why the other differs. */
    for (int i = 0; i < 4; i++) { (void)now(); (void)time_get_us(); }

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

/* --- ESP32-P4: the core-local CLINT, behind the CLIC ------------------ */
#elif defined(CONFIG_BOARD_ESP32P4)

/* E4, plan/phase27_esp32p4_bringup.md.
 *
 * Registers from TRM section 2.9.3.5's summary; the layout below is the
 * *core-local* window, "CLINT (self)", so hart 0 and hart 1 each address
 * their own comparator through the same constant (section 2.8.3, Table
 * 2.8-2). The other core's block is at 0x20010000 and this file never wants
 * it -- except for one bit, noted at MTIME_EN.
 *
 * mtimelo/mtimehi are at 0xBFF8/0xBFFC, which look like the standard CLINT
 * offsets and are; mtimecmp is *not* -- it is at 0x4000/0x4004 rather than
 * the usual 0x4000 + 8*hart, because this block is per-core rather than
 * one block indexed by hart. Reading them as a single 64-bit access would
 * also be wrong: they are documented as two 32-bit registers, and the
 * sampling mode below exists precisely because the pair cannot be read
 * atomically. */
#define P4_CLINT_BASE   0x20000000UL
#define P4_MTIMECMPLO   (*(volatile uint32_t *)(P4_CLINT_BASE + 0x4000))
#define P4_MTIMECMPHI   (*(volatile uint32_t *)(P4_CLINT_BASE + 0x4004))
#define P4_MTIMECTL     (*(volatile uint32_t *)(P4_CLINT_BASE + 0x4010))
#define P4_MTIMELO      (*(volatile uint32_t *)(P4_CLINT_BASE + 0xBFF8))
#define P4_MTIMEHI      (*(volatile uint32_t *)(P4_CLINT_BASE + 0xBFFC))

/* mtimectl (TRM Register 2.105), read off the rendered bit diagram rather
 * than the text: MTIME_EN bit 0, MTIME_OVF bit 1, MTIME_SAM bits [3:2].
 *
 * MTIME_EN's reset value is 1, not 0. Worth stating because E0's note in the
 * phase plan said "the counter does not run until told to ... structurally
 * the same surprise RP2350 had", and it is not: the RP2350's tick generator
 * really does reset disabled, this one resets running. It is still written
 * explicitly, for the reason drivers/uart_esp32p4.c gives about the console
 * -- inheriting a state because the ROM happened to leave it is not a
 * property this image gets to keep when E6 boots it a different way.
 *
 * MTIME_SAM stays at 0, and that is a reversal worth recording: E0's reading
 * of the plan said this arm "should use it rather than copying the RP2350
 * path's re-read loop". It should not, and finding out cost this milestone
 * an afternoon -- see the block comment on now() below. */
#define P4_MTIME_EN         (1u << 0)
#define P4_MTIME_OVF        (1u << 1)
#define P4_MTIME_SAM_SHIFT  2
#define P4_MTIME_SAM_MASK   (3u << P4_MTIME_SAM_SHIFT)

/* Nominal only. The TRM documents every register of this block and never
 * names its source, and ESP-IDF is no help because it does not use the CLINT
 * on this chip at all (it ticks FreeRTOS off the systimer; its own
 * CLINT_BASE constant, 0x02000000, is the generic RISC-V one and unrelated
 * to this window).
 *
 * **It follows the CPU clock**, established by phase 34 rather than by
 * reading: at 40 MHz the crystal and the CPU were the same number and the
 * question could not be asked, and the moment 34.4 put HP_ROOT_CLK on the
 * PLL this counter moved with it -- measured 79 964 535 Hz at 80 MHz and
 * 360 070 000 Hz at 360.
 *
 * Which makes the measurement below load-bearing rather than tidy: it is
 * what keeps preemption at 100 Hz across a clock change, and it does. This
 * value is only what ticker_init() divides by before the measurement
 * replaces it, and the systimer's 16 MHz is as good a starting guess as
 * any. */
#define TICK_HZ 16000000UL

/* The 64-bit counter, read the same way every other target here reads its
 * own: sample, re-read the high half, retry if it moved.
 *
 * The first version of this used the hardware's own sampling mode instead,
 * MTIME_SAM = 3, because TRM Register 2.105 offers exactly that and E0's
 * survey recommended it. Read the wording again, though: "Sample the other
 * half of the system count on reading MTIMELO **or** MTIMEHI". Either read
 * latches the other half. So a pair of reads gets one live value and one
 * latched value, and the *next* call reads a half that was latched during
 * the previous call -- the two halves of one returned value no longer come
 * from the same instant.
 *
 * That is invisible for the first wrap of uptime, and only then. The low
 * half wraps at 2^32 ticks -- 107 seconds at 40 MHz, and **11.9 seconds at
 * the 360 MHz phase 34 took this board to**, since this counter follows the
 * CPU. Until it wraps the high half
 * is 0 and a stale 0 is indistinguishable from a fresh one. After the wrap a
 * mixed read can return a value 2^32 ticks -- 107 seconds -- away from the
 * truth, and ticker_next()'s set_deadline(now() + interval) then arms the
 * comparator a minute and a half into the future.
 *
 * Which is exactly what the board did: preemption worked, `lockselftest`
 * passed at t = 18 s, and `preempttest` at t = 208 s reported **ticks=1** --
 * one tick, the one already armed, and then nothing. A preemption timer that
 * works for the first hundred seconds of every boot and then quietly stops
 * is a considerably worse bug than one that never works, because every quick
 * test passes.
 *
 * The loop costs one extra load in the case where the high half moved, which
 * is once every wrap -- 11.9 seconds at 360 MHz. There is no version of
 * this where the hardware
 * mode was worth it. */
static uint64_t now(void) {
    uint32_t hi, lo;
    do { hi = P4_MTIMEHI; lo = P4_MTIMELO; } while (hi != P4_MTIMEHI);
    return ((uint64_t)hi << 32) | lo;
}

static void set_deadline(uint64_t t) {
    /* High half to all-ones first, so the 64-bit comparator can never
     * momentarily hold a value in the past while half-updated and fire a
     * spurious interrupt. Same three writes, same order, and the same reason
     * as the RP2350 arm above -- this comparator is two 32-bit registers
     * too. */
    P4_MTIMECMPHI = 0xffffffffu;
    P4_MTIMECMPLO = (uint32_t)t;
    P4_MTIMECMPHI = (uint32_t)(t >> 32);
}

static bool arch_ticker_init(void) {
    /* Sampling off, counter on, overflow flag cleared, in one write.
     *
     * MTIME_SAM is written to 0 rather than left alone: now() above depends
     * on the counter halves being live, and "the ROM probably left it at its
     * reset value" is the kind of assumption this port has already been
     * burned by. MTIME_OVF is write-0-to-clear and writing 1 has no effect,
     * so clearing it costs nothing and means a counter that wrapped before
     * this boot does not carry a stale flag into it. */
    uint32_t ctl = P4_MTIMECTL;
    ctl &= ~(P4_MTIME_SAM_MASK | P4_MTIME_OVF);
    P4_MTIMECTL = ctl | P4_MTIME_EN;

    /* Measure the rate rather than assume it, against the microsecond clock
     * kernel/time.c already provides -- the systimer, whose own divisor E2
     * checked against a host wall clock over sixty seconds.
     *
     * This is also the answer to the two-time-bases warning this milestone
     * was handed. The concern was that the tick would run off mtime while
     * the clock ran off the systimer, giving any timing anomaly two
     * independent suspects. Measuring the interval *against* time.c removes
     * the independence: the tick is expressed in whatever mtime's unit turns
     * out to be, but its length is derived from the clock this system
     * already tells the time with. If the systimer is wrong, the tick is
     * wrong in exactly the same proportion, which is the failure mode that
     * shows up as one discrepancy rather than two.
     *
     * 2 ms because that is long enough to divide by and short enough not to
     * be noticed at boot; the RP2350 arm uses the same window for the same
     * reason. */
    /* Warm the measurement path before opening the window.
     *
     * Not a superstition and not padding: **measured on the board, 34.1 and
     * the fix in 34.2a** (plan/phase34_esp32p4_pll_bringup.md). The same
     * 2 ms window, on the same silicon, reads
     *
     *     41 118 086 Hz   from here, at boot
     *     39 999 500 Hz   from a shell command, warm
     *     40 000 029 Hz   over a full second, warm
     *
     * so the window length was never the problem. The bias is the *first*
     * call's instruction fetch. Since phase 32 this code executes from flash
     * over a 20 MHz MSPI, and `t0 = now()` is sampled before the first
     * `time_get_us()` -- so the cold fetch of that first call lands *inside*
     * the tick window and *outside* the microsecond window. The two windows
     * stop being the same length, and the ratio is the measurement.
     *
     * 4.07% of 2 ms is 81 us, which is what a cold XIP call costs here. It
     * made the preemption tick run at 95.99 Hz against the 100 Hz asked for,
     * measured directly as 6192 ticks over 64.51 s -- and it was invisible
     * for a phase because nothing printed two measurements of one clock side
     * by side.
     *
     * A few calls through both paths puts their lines in cache, after which
     * the symmetry the code was always written for actually holds. Both
     * functions read volatile MMIO, so neither the calls nor the loop can be
     * optimised away. */
    for (int i = 0; i < 4; i++) { (void)now(); (void)time_get_us(); }

    uint64_t t0 = now();
    uint64_t us0 = time_get_us();
    while (time_get_us() - us0 < 2000) { /* spin */ }
    uint64_t elapsed_ticks = now() - t0;
    uint64_t elapsed_us = time_get_us() - us0;

    if (elapsed_ticks == 0 || elapsed_us == 0) {
        /* MTIME_EN resets to 1 and was just written again, so a frozen
         * counter here is not a forgotten enable -- it is this core's CLINT
         * not being clocked, which is a finding rather than a bug to work
         * around. Refuse, and say which of the two it is. */
        printk("[Ticker] ESP32-P4: mtime is not advancing (mtimectl=0x%lx); "
               "preemption stays off\n", (unsigned long)P4_MTIMECTL);
        return false;
    }

    uint64_t measured_hz = (elapsed_ticks * 1000000UL) / elapsed_us;
    g_measured_hz = measured_hz;
    g_interval = measured_hz / g_hz;
    if (g_interval == 0) g_interval = 1;

    set_deadline(now() + g_interval);

    /* And the CLIC, because mie does not exist here. TRM section 2.9.2.1:
     * "the basic RISC-V interrupt handling scheme and the associated CSRs
     * (such as mie, mip, mideleg, uie, and uip) are unavailable". The
     * set_csr(mie, 1 << 7) that every other branch of this file ends with
     * would assemble, execute, and do nothing -- which is why this arm
     * refused to arm anything at all until the controller existed. */
    esp32p4_clic_timer_enable();
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
#if defined(CONFIG_BOARD_ESP32P4)
    /* Not set_csr(mie, MTIE): that CSR does not exist on this core, and the
     * write would assemble and do nothing. Each core has its own CLIC, so
     * this enables the caller's -- which is the whole point of this function
     * being per-hart. Unreachable today (CONFIG_ENABLE_SMP is off on this
     * board) and correct when it is not. */
    esp32p4_clic_timer_enable();
#elif defined(CONFIG_MODE_S)
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
