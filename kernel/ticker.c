#include "kernel/ticker.h"
#include "kernel/printk.h"
#include "arch/csr.h"
#include "arch/trap.h"

/* See kernel/include/kernel/ticker.h for why this has three backends. */

static bool     g_enabled;
static uint64_t g_ticks;
static uint64_t g_interval;   /* in whatever unit the target's clock counts */

bool     ticker_enabled(void) { return g_enabled; }
uint64_t ticker_ticks(void)   { return g_ticks; }
void     ticker_count_tick(void) { g_ticks++; }

/* --- RP2350: SIO mtime/mtimecmp -------------------------------------- */
#if defined(CONFIG_BOARD_RP2350)

#define SIO_BASE        0xd0000000UL
#define SIO_MTIME_CTRL  (*(volatile uint32_t *)(SIO_BASE + 0x1a4))
#define SIO_MTIME       (*(volatile uint32_t *)(SIO_BASE + 0x1b0))
#define SIO_MTIMEH      (*(volatile uint32_t *)(SIO_BASE + 0x1b4))
#define SIO_MTIMECMP    (*(volatile uint32_t *)(SIO_BASE + 0x1b8))
#define SIO_MTIMECMPH   (*(volatile uint32_t *)(SIO_BASE + 0x1bc))
#define MTIME_CTRL_EN   (1u << 0)

/* The SDK documents this timer as ticking at 1 MHz when not in FULLSPEED. */
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
    /* DISABLED pending investigation -- see the B6 notes in
     * plan/phase5_distributed_design.md.
     *
     * Enabling the SIO mtime comparator and MTIE wedges the board: it stops
     * echoing console input partway through a line and the physical UART goes
     * silent too, which is a trap loop rather than a slow system. The same
     * code preempts correctly on both QEMU targets, so it is something
     * specific to Hazard3's platform timer or how RP2350 routes it, and I do
     * not yet understand which.
     *
     * Refusing is the honest state: preemption reports itself off (ticker_
     * enabled() is false, and `preempttest` says so), the board stays usable,
     * and cooperative scheduling continues to work exactly as it did through
     * B0-B5. Shipping a kernel that hangs the only physical target would be
     * strictly worse than shipping one that says it cannot preempt yet. */
    (void)set_deadline;
    (void)now;
    printk("[Ticker] Preemption disabled on RP2350 (platform timer under investigation)\n");
    return false;
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

bool ticker_init(uint32_t hz) {
    if (hz == 0) return false;
    g_interval = TICK_HZ / hz;
    if (g_interval == 0) g_interval = 1;

    if (!arch_ticker_init()) {
        g_enabled = false;
        return false;
    }
    g_enabled = true;
    printk("[Ticker] Preemption timer at %u Hz (%lu ticks of a %lu Hz clock)\n",
           (unsigned)hz, (unsigned long)g_interval, (unsigned long)TICK_HZ);
    return true;
}
