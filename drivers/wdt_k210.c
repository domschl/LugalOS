/*
 * LugalOS driver: the hardware watchdog on the K210 -- 65.7.
 *
 * An unattended node must not stay hung. WDT0 (0x50400000, an ARM
 * SP805-class block -- the vendor feed value 0x76 is SP805's) counts
 * and expires on this silicon, a task at normal priority feeds it a
 * quarter of the range before expiry, and expiry resets the chip.
 *
 * Measured on the board, because the vendor SDK and its header are
 * quietly wrong in two places:
 *
 *   * RMOD=0 ("response: reset") never fires. Verified live with
 *     counter running, clocks on, mode 0 -- both with and without bit2
 *     of CR (the SP805's RREN, which the Canaan header omits). RMOD=1
 *     (interrupt) does fire. So the timeout takes the interrupt path:
 *     the handler prints the dying words, arms one final range (a
 *     wedged response gives a second interrupt, not a reset loop --
 *     a live board saying so), and issues SYSCTL soft_reset (bit0 of
 *     +0x30), which the reset_status latch duly reports as "software"
 *     on the next boot.
 *   * The clock law promises xtal/((threshold+1)*2) = 13 MHz; the
 *     counter measures ~6.5 MHz with threshold reading 0. arm()
 *     therefore counts CCVR against CLINT mtime (7.8125 MHz, 65.1)
 *     and derives the real rate -- no frequency constant is trusted.
 *
 * The reset_source latch (SYSCTL+0x60: pin / wdt0 / wdt1 / soft) is
 * read once at boot before clearing; a WDT-response reset latches as
 * `pin` on this part, so the latch is reported for what it is and the
 * watchdog's own story rides on the console line it prints.
 */

#include "drivers/watchdog.h"
#include "kernel/devirq.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REG(a)   (*(volatile uint32_t *)(uintptr_t)(a))
#define REG64(a) (*(volatile uint64_t *)(uintptr_t)(a))

#define K210_CLINT_MTIME    0x0200BFF8UL        /* low word; ms-scale windows never wrap it */

/* WDT1, not WDT0: the Kendryte interrupt table numbers WDT0 as PLIC
 * source 0, and a PLIC claim of 0 means "nothing" -- WDT0's timeout is
 * physically generated and logically undeliverable. WDT1 (source 1) is
 * the same block with a claimable id. */
#define K210_WDT0_BASE      0x50410000UL
#define WDT_CR              (K210_WDT0_BASE + 0x00u)
#define WDT_TORR            (K210_WDT0_BASE + 0x04u)
#define WDT_CVR             (K210_WDT0_BASE + 0x08u)
#define WDT_CCR             (K210_WDT0_BASE + 0x0cu)
#define WDT_EOI             (K210_WDT0_BASE + 0x14u)
#define WDT_CR_ENABLE       (1u << 0)
#define WDT_CR_RMOD_INT     (1u << 1)
#define WDT_FEED_MAGIC      0x76u

#define K210_SYSCTL         0x50440000UL
#define SYSCTL_CLK_EN_CENT  (K210_SYSCTL + 0x28u)
#define SYSCTL_CLK_EN_PERI  (K210_SYSCTL + 0x2cu)
#define SYSCTL_SOFT_RESET   (K210_SYSCTL + 0x30u)
#define SYSCTL_PERI_RESET   (K210_SYSCTL + 0x34u)
#define SYSCTL_RESET_STATUS (K210_SYSCTL + 0x60u)
#define APB1_CLK_EN         (1u << 4)
#define WDT0_CLK_EN         (1u << 25)   /* wdt1_clk_en */
#define WDT0_PERI_RESET     (1u << 26)   /* wdt1_reset */
#define RST_STS_CLR         (1u << 0)
#define RST_STS_PIN         (1u << 1)
#define RST_STS_WDT0        (1u << 2)
#define RST_STS_WDT1        (1u << 3)
#define RST_STS_SOFT        (1u << 4)

/* PLIC (k210: WDT0 is source 0; the K210 PLIC is the T-Head shape --
 * one claim_complete register -- and drivers/uart_k210.c's bring-up
 * already taught trap.c to service level sources from it). */
#define IRQ_WDT0            1u   /* WDT1 == PLIC source 1; see the base register note */
#define K210_PLIC           0x0C000000UL
#define PLIC_PRIORITY(p)    (K210_PLIC + 4u * (uint32_t)(p))
#define PLIC_ENABLE         (K210_PLIC + 0x2000u)
#define PLIC_THRESHOLD      (K210_PLIC + 0x200000u)

#define WDT_REQUEST_MS      30000u
#define MT_HZ               7812500ull          /* CLINT mtime, measured 65.1 */

/* single task: the boot, constant after arm(); g_real_ms is written
 * once by arm() before the feeder or the IRQ can read it. */
static uint32_t g_top;
static uint32_t g_real_ms;
static uint32_t g_rate_hz;
/* guarded by: the feeder task writes g_feeds; reports read stale.
 * g_withhold is set from the shell task -- a single bool, and losing a
 * race of one iteration is within the test's tolerance by design. */
static volatile bool g_armed, g_withhold;
static volatile bool g_fired;
/* single task: the boot, before any reader runs; constant after. */
static uint32_t g_reason_bits;
static const char *g_reason_str;
static uint32_t g_feeds;

static void wdt_feed(void) {
    REG(WDT_CCR) = WDT_FEED_MAGIC;
    g_feeds++;
}

static void wdt_arm_at(uint32_t top) {
    g_top = top;
    REG(WDT_CR) = 0;                            /* off while reconfiguring */
    REG(WDT_TORR) = (top << 4) | top;           /* TOP written twice (SP805 habit) */
    REG(WDT_CCR) = WDT_FEED_MAGIC;              /* load the counter */
    REG(WDT_CR) = WDT_CR_ENABLE | WDT_CR_RMOD_INT;
}

static void wdt_arm(void) {
    /* Clocks: apb1 carries the WDT clock one gate above its own enable
     * (the SDK sets both), then a reset pulse on the block. */
    REG(SYSCTL_CLK_EN_CENT) |= APB1_CLK_EN;
    REG(SYSCTL_CLK_EN_PERI) |= WDT0_CLK_EN;
    REG(SYSCTL_PERI_RESET) |= WDT0_PERI_RESET;
    for (volatile int i = 0; i < 8; i++) { }
    REG(SYSCTL_PERI_RESET) &= ~WDT0_PERI_RESET;

    /* The TOP the vendor law asks for at its promised 13 MHz. */
    uint64_t cycles = (uint64_t)WDT_REQUEST_MS * 13000ull;    /* ms * 13e3 cycles/ms */
    uint64_t scaled = cycles >> 16;
    uint32_t top = 0;
    while ((scaled >> 1) != 0 && top < 15u) { top++; scaled >>= 1; }
    wdt_arm_at(top);

    /* Then the truth: count CCVR down against mtime. CCVR reloads on
     * feed and counts DOWN (SP805), so the delta is c0 - c1; one wrap
     * inside a 200 ms window is possible only at absurd rates and the
     * two candidate deltas tell which one happened. */
    task_sleep_ms(200u);
    uint64_t m0 = REG64(K210_CLINT_MTIME);
    uint32_t c0 = REG(WDT_CVR);
    task_sleep_ms(200u);
    uint64_t m1 = REG64(K210_CLINT_MTIME);
    uint32_t c1 = REG(WDT_CVR);
    uint64_t dm = m1 - m0;
    if (dm == 0) dm = 1;
    uint64_t dc = (uint32_t)(c0 - c1);
    uint64_t rate_a = dc * MT_HZ / dm;
    uint64_t rate_b = (dc + (1ull << (16 + top))) * MT_HZ / dm;
    g_rate_hz = (uint32_t)((rate_a > 1000000ull && rate_a < 60000000ull) ? rate_a : rate_b);
    g_real_ms = (uint32_t)(((uint64_t)1u << (16 + top)) * 1000ull / g_rate_hz);
    if (g_real_ms < 1000u) g_real_ms = 1000u;   /* never let a bad measurement make a tick-tight watchdog */

    /* PLIC: WDT0 above the threshold, unmasked for this hart. */
    REG(PLIC_PRIORITY(IRQ_WDT0)) = 8u;
    if (REG(PLIC_THRESHOLD) < 4u) REG(PLIC_THRESHOLD) = 4u;
    REG(PLIC_ENABLE) |= (1u << IRQ_WDT0);
}

/* PLIC source 0, dispatched by arch/riscv/common/trap.c's claim loop
 * once devirq_attach has bound it. Trap context: only printk_critical
 * (which the trap path itself uses) and bare register writes. */
static void wdt_isr(void *ctx) {
    (void)ctx;
    REG(WDT_EOI) = 1;
    g_fired = true;
    printk_critical("[WDT] TIMEOUT: no feed for %u s -- resetting the chip\n",
                    (unsigned)g_real_ms);
    /* One final range at the tightest the block does. If the response
     * path is wedged this handler re-enters forever; a live board
     * saying so beats a reset loop. */
    wdt_arm_at(0);
    REG(SYSCTL_SOFT_RESET) = 1u;
    for (;;) { }
}

const char *watchdog_reset_reason(void) { return g_reason_str; }

bool watchdog_last_reset_was_watchdog(void) {
    return (g_reason_bits & (RST_STS_WDT0 | RST_STS_WDT1)) != 0;
}

static void wdog_task(void *arg) {
    (void)arg;
    for (;;) {
        if (!g_withhold) wdt_feed();
        task_sleep_ms(g_real_ms / 4u);            /* g_real_ms has a 1 s floor: this never spins */
    }
}

void watchdog_start(void) {
    /* Handler first: the line is enabled by arm() the moment this
     * returns, and an attached-but-quiet source beats a live one with
     * nobody listening (which the trap path masks on sight). */
    if (devirq_attach(IRQ_WDT0, wdt_isr, NULL) != 0) {
        printk("[WDT] WDT0 source busy -- watchdog left off\n");
        return;
    }
    /* Latch and clear the reset source before anything else can set or
     * consume it. Honest labels: the WDT *response* resets through the
     * pad domain and latches as `pin` here (measured); this driver's
     * own resets latch as `software`. The unattended story is the
     * TIMEOUT line on the console followed by recovery. */
    uint32_t sts = REG(SYSCTL_RESET_STATUS);
    REG(SYSCTL_RESET_STATUS) = RST_STS_CLR;
    g_reason_bits = sts & 0x1Eu;
    g_reason_str = (sts & RST_STS_WDT0) ? "watchdog (WDT0)" :
                   (sts & RST_STS_WDT1) ? "watchdog (WDT1)" :
                   (sts & RST_STS_SOFT) ? "software" :
                   (sts & RST_STS_PIN)  ? "pin" : "power-on";
    printk("[WDT] last reset: %s (0x%02x)\n", g_reason_str, (unsigned)g_reason_bits);

    wdt_arm();
    /* The feeder shares no state with the shell beyond the bools
     * above; if creating it fails, a watchdog with no feeder would
     * reset a healthy board, so disarm rather than gamble. */
    if (task_create_sized("wdog", wdog_task, NULL, 1) < 0) {   /* one page: it sleeps and writes a register */
        REG(WDT_CR) = 0;
        REG(PLIC_ENABLE) &= ~(1u << IRQ_WDT0);
        printk("[WDT] could not create the feeder task -- watchdog left off\n");
        return;
    }
    g_armed = true;
    printk("[WDT] armed: %u s timeout at %u Hz (top %u), soft reset on expiry\n",
           (unsigned)g_real_ms / 1000u, (unsigned)g_rate_hz, (unsigned)g_top);
}

void watchdog_report(void) {
    printk("watchdog: %s, timeout %u s, %u feeds%s%s\n", g_armed ? "armed" : "off",
           (unsigned)(g_real_ms / 1000u), (unsigned)g_feeds, g_withhold ? ", feeding WITHHELD" : "",
           g_fired ? ", fired this boot" : "");
    printk("last reset: %s (0x%02x)\n", watchdog_reset_reason(), (unsigned)g_reason_bits);
}

uint32_t watchdog_proc_render(char *out, uint32_t cap) {
    int n = ksnprintf(out, cap, "armed=%s\ntimeout_s=%u\nfeeds=%u\nwithheld=%s\nfired=%s\nlast_reset=%s\n"
                      "last_reset_code=0x%02x\nrestarted_itself=%s\n",
                      g_armed ? "yes" : "no", (unsigned)(g_real_ms / 1000u), (unsigned)g_feeds,
                      g_withhold ? "yes" : "no", g_fired ? "yes" : "no",
                      watchdog_reset_reason(), (unsigned)g_reason_bits,
                      watchdog_last_reset_was_watchdog() ? "yes" : "no");
    return (uint32_t)n < cap ? (uint32_t)n : cap;
}

/* `wdt test`: stop feeding, as a hung scheduler would. The board
 * resets within the timeout -- the whole path, proven on the hardware. */
void watchdog_withhold(void) {
    g_withhold = true;
    printk("[WDT] feeding withheld: expect a reset within %u s\n",
           (unsigned)(g_real_ms / 1000u));
}
