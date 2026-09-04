/*
 * LugalOS Hardware Driver: PL011 UART0 + Dual Onboard/External LEDs for RP2350 (Pico 2)
 * Adopted from working bare-metal RP2350 driver (rp2350_cld).
 *
 * Hardware Map (K2, plan/phase7_kernel_config.md -- the numbers themselves
 * live in cmake/board-rp2350.cmake now, this is documentation, not the
 * source of truth):
 *   GP0  : UART0 TX (Function 2)
 *   GP1  : UART0 RX (Function 2)
 *   GP25 : Onboard LED (Function 5 - SIO)
 *   GP16 : External LED (Function 5 - SIO, active-high pulse)
 */

#include "drivers/uart.h"
#include "drivers/uart_net.h"
#include "fs/p9_link.h"
#include "kernel/sched.h"
#include "kernel/palloc.h"
#include "kernel/time.h"
#include "kernel/devirq.h"
#include "kernel/irq.h"
#include "kernel/lock.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include "kernel/mem_domain.h"
#include "kernel/device.h"
#include "kernel/ipc.h"
#include "arch/trap.h"
#include "arch/umode.h"
#include "lugalos_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define CLOCKS_BASE             0x40010000UL
#define RESETS_BASE             0x40020000UL
#define RESETS_RESET_DONE       (RESETS_BASE + 0x08)
#define RESETS_ATOMIC_CLEAR     (RESETS_BASE + 0x3000)

#define IO_BANK0_BASE           0x40028000UL
#define IO_BANK0_CTRL(n)        (IO_BANK0_BASE + 0x004 + (n) * 8)

#define PADS_BANK0_BASE         0x40038000UL
#define PADS_BANK0_PAD(n)       (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define SIO_BASE                0xD0000000UL
#define SIO_GPIO_OUT_SET        (SIO_BASE + 0x018)
#define SIO_GPIO_OUT_CLR        (SIO_BASE + 0x020)
#define SIO_GPIO_OE_SET         (SIO_BASE + 0x038)

/* M5, plan/phase12_microkernel_migration.md: RP2350's Arm TrustZone-style
 * Secure/Non-secure split, mapped onto RISC-V M-mode/U-mode -- found in
 * ~/gith/pico/datasheet's RP2350 datasheet (Section 3.1.1, "Secure and
 * Non-secure SIO") after the heartbeat-to-U-mode conversion below wrote to
 * SIO_GPIO_OUT_SET from U-mode with no fault and no effect. SIO's GPIO
 * output/input registers are *shared* silicon (not truly duplicated the
 * way SIO's FIFOs/doorbells/spinlocks are), but a Non-secure (U-mode) bus
 * access is filtered per-GPIO by ACCESSCTRL's GPIO_NSMASK0/1 registers: "the
 * bit is read-only zero" for any GPIO not marked accessible -- exactly the
 * silently-ignored-write symptom this was. PMP has nothing to say about
 * this: it is a separate hardware filter, upstream of PMP, that a U-mode
 * task's own domain cannot grant its way around -- the mask has to be set
 * from Secure (M-mode) code before the task ever tries. Defaults to 0 (all
 * GPIOs Secure-only) on reset, so every board persona has been running
 * with every GPIO Secure-only the whole time -- invisible until now because
 * nothing had ever touched a GPIO from U-mode before. */
#define ACCESSCTRL_BASE          0x40060000UL
#define ACCESSCTRL_GPIO_NSMASK0  (ACCESSCTRL_BASE + 0x0c) /* GPIO 0-31 */

/* M5 Phase 6, plan/phase12_microkernel_migration.md: same per-peripheral
 * register shape as every non-GPIO ACCESSCTRL register this project has
 * touched (SPI0/SPI1/I2C0/I2C1) -- checked directly against
 * ~/gith/pico/pico-sdk's accessctrl.h: offset 0xa0, needs the
 * 0xacce0000 write-password prefix. */
#define ACCESSCTRL_UART0         (ACCESSCTRL_BASE + 0xa0)
#define ACCESSCTRL_UART0_NSP     (1u << 1)
#define ACCESSCTRL_UART0_NSU     (1u << 0)
#define ACCESSCTRL_WRITE_PASSWORD 0xacce0000UL

#define UART0_BASE              ((uintptr_t)CONFIG_UART0_BASE)

#define REG(addr) (*(volatile uint32_t *)(addr))

/* The onboard-LED pin is optional: on the clock persona (a Pico 2 W) there is
 * no such pin to name -- the user LED is on the wireless module and GP25 is
 * its chip select -- so that board file omits the key and the heartbeat is
 * GP9 alone (phase17 C1). */
#ifdef CONFIG_LED_ONBOARD_GPIO
#define LED_MASK  ((1u << CONFIG_LED_ONBOARD_GPIO) | (1u << CONFIG_LED_EXT_GPIO))
#else
#define LED_MASK  (1u << CONFIG_LED_EXT_GPIO)
#endif

#include "drivers/usb_cdc.h"

static inline uint32_t read_mcycle(void) {
    uint32_t c;
    __asm__ volatile("csrr %0, mcycle" : "=r"(c));
    return c;
}

void led_on(void) {
    REG(SIO_GPIO_OUT_SET) = LED_MASK;
}

void led_off(void) {
    REG(SIO_GPIO_OUT_CLR) = LED_MASK;
}

/* Heartbeat on GP16: 0.5 Hz, brief flash.
 *
 * M4.5, plan/phase12_microkernel_migration.md, Part B: this used to be a
 * two-state machine stepped as a side effect of uart_has_char()/
 * uart_getc()'s console-polling loop (a function called gp16_alive_tick(),
 * folded into that poll specifically so a blocking spin here could not
 * starve USB CDC servicing -- see the git history of this file for that
 * fix). That made the heartbeat a proxy for "is anything polling the
 * console" rather than "is the scheduler giving every READY task its
 * turn" -- a program that reads no input for an extended stretch (uspin,
 * for one) would silently stop the heartbeat too, which looks exactly
 * like the scheduler problem this LED exists to help rule out, for a
 * reason that has nothing to do with scheduling.
 *
 * A real, independently-scheduled task: it blinks at its own rate
 * regardless of what any other task is doing, which is what makes it an
 * actual live, on-the-board check that the scheduler is working -- exactly
 * the M4.5 Part A question (does every READY task get a turn), but
 * continuously, visually, and without a serial connection. TASK_PRIO_
 * NORMAL, not TASK_PRIO_INTERRUPT: the point is to reflect ordinary
 * scheduling health, including whether NORMAL-tier tasks get their turn
 * under load, not to guarantee a smooth blink regardless of it -- an
 * INTERRUPT-tier heartbeat would always look healthy and prove nothing.
 * Sleeps by yielding, not spinning, between checks -- a background
 * indicator has no business burning CPU that another READY task could use
 * while it waits out 2 seconds of mostly being off.
 *
 * M5, plan/phase12_microkernel_migration.md: this task's own loop runs in
 * real U-mode now, not kernel mode like every other M4.5 driver task --
 * the deliberate first (and, for this milestone, only) instance of that,
 * chosen because it needs no chan_call() endpoint and touches exactly one
 * register pair, so it exercises the new mechanism without also needing
 * the chan_serve_wait()/reply() syscalls every *other* driver task would.
 * See that milestone's own plan notes for why a domain attached to a
 * kernel-mode task (M5 as originally, too narrowly, scoped) enforces
 * nothing at all on this board: PMP does not restrict M-mode, which is
 * where every other driver task in this tree still runs.
 *
 * Same UATTR/.utext mechanism kernel/shell.c's usertest/intruder/deputytest
 * probes already use (not the separately linked ELF-loader path C4 built --
 * that is for programs loaded from a file after the filesystem exists;
 * this task starts long before vfs_server_init() runs, same boot-ordering
 * constraint every other M4.5 driver task already has). Unlike those
 * probes, this task never exits, so it gets its own dedicated stack rather
 * than sharing kernel/shell.c's g_user_stack (which assumes exactly one
 * such probe is ever in flight at a time). */
#define HEARTBEAT_PERIOD_MS 2000u  /* 0.5 Hz */
#define HEARTBEAT_ON_MS       40u  /* brief flash, not a duty cycle */

/* Same attribute shell.c's own UATTR uses, redefined locally rather than
 * shared: a section attribute is exactly the two lines below, and sharing
 * it through a header would suggest the two files' U-mode probes are more
 * coupled than they are. no_sanitize("undefined"): UBSan's own instrumented
 * checks are kernel .text calls, unreachable from a page U-mode can execute
 * but the kernel does not treat as its own .text. */
#define HEARTBEAT_UATTR __attribute__((section(".utext"))) __attribute__((no_sanitize("undefined")))

/* Hand-rolled per translation unit, not shared with kernel/shell.c's
 * usys_*() or user/progs/usys.h: a HEARTBEAT_UATTR function must not call
 * anything the compiler might place outside .utext, and a cross-file (or
 * even cross-function, without always_inline) inline is not a guarantee --
 * shell.c's own usys_putc() comment found this out directly at -Os. */
__attribute__((always_inline)) static inline long heartbeat_usys_yield(void) {
    register long r_a0 __asm__("a0") = SYS_YIELD;
    __asm__ __volatile__("ecall" : "+r"(r_a0) :: "memory");
    return r_a0;
}
__attribute__((always_inline)) static inline long heartbeat_usys_time_ms(void) {
    register long r_a0 __asm__("a0") = SYS_TIME_MS;
    __asm__ __volatile__("ecall" : "+r"(r_a0) :: "memory");
    return r_a0;
}

/* Found on real hardware, not predicted: a plain `static void`, neither
 * always_inline nor HEARTBEAT_UATTR, is not a guarantee the compiler places
 * it (or keeps it placed) in .utext -- it was correctly inlined into
 * heartbeat_umode_body() during initial bring-up, but the moment this
 * function's body grew (a debug-only iteration counter, since removed), -Os
 * stopped inlining it and placed it in ordinary kernel .text instead. The
 * result on real RP2350 hardware: heartbeat_umode_body() branching into
 * kernel .text (outside this task's granted RX region) took an instruction
 * access fault (cause 1) and the task was correctly terminated -- exactly
 * B3's isolation mechanism doing its job, just not the job intended here.
 * HEARTBEAT_UATTR makes the placement structural rather than an inlining
 * heuristic's decision, the same discipline every other .utext function in
 * this file and kernel/shell.c already follows. */
HEARTBEAT_UATTR static void heartbeat_usleep_until(long target_ms) {
    while (heartbeat_usys_time_ms() < target_ms) heartbeat_usys_yield();
}

/* The real, production heartbeat loop -- runs forever in U-mode, so unlike
 * every probe in kernel/shell.c it never reaches an exit syscall. GPIO
 * writes stay plain stores: no syscall needed, governed entirely by this
 * task's own domain (see heartbeat_task_start() below) and by GP16 being
 * marked Non-secure-accessible in ACCESSCTRL (see uart_init()'s own
 * comment on that) -- which is the whole point of granting a device
 * window rather than routing every register access through the kernel. */
HEARTBEAT_UATTR static void heartbeat_umode_body(void) {
    for (;;) {
        REG(SIO_GPIO_OUT_SET) = (1u << CONFIG_LED_EXT_GPIO);
        heartbeat_usleep_until(heartbeat_usys_time_ms() + HEARTBEAT_ON_MS);
        REG(SIO_GPIO_OUT_CLR) = (1u << CONFIG_LED_EXT_GPIO);
        heartbeat_usleep_until(heartbeat_usys_time_ms() + (HEARTBEAT_PERIOD_MS - HEARTBEAT_ON_MS));
    }
}

/* Own dedicated stack, not shared with kernel/shell.c's one-shot test
 * probes (see this section's own comment above for why). M5 heap-reclaim
 * (plan/phase12_microkernel_migration.md): 256 bytes, not a full page --
 * heartbeat_umode_body() makes no calls at all post-inlining (measured
 * deepest stack use: 0 bytes), so 256 is pure headroom. This file is
 * RP2350-only, so (unlike kernel/shell.c's g_user_stack) there is no
 * Sv39-page-granularity reason to keep this at a full page. See
 * .ustacks256's own comment in linker/rp2350.ld for the grouping. */
static uint8_t g_heartbeat_ustack[256] __attribute__((aligned(256)))
                                        __attribute__((section(".ustacks256")));
static mem_domain_t g_heartbeat_domain;

/* This task's own kernel-mode entry point: task_create_sized() calls this
 * (ordinary kernel stack, kernel privilege) to build the domain and make
 * the one-way jump into U-mode. Mirrors kernel/shell.c's
 * user_task_common()/run_user_task() shape exactly. */
static void heartbeat_task_body(void *arg) {
    (void)arg;

    mem_domain_init(&g_heartbeat_domain);

    /* Order matters: PMP resolves against the lowest-numbered matching
     * region, so narrower grants must precede broader ones that might
     * overlap them (none do here, but the ordering convention is kept for
     * consistency with every other domain construction in this tree). */
    mem_domain_add(&g_heartbeat_domain, (uintptr_t)g_heartbeat_ustack,
                   sizeof(g_heartbeat_ustack), MEM_R | MEM_W);

    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&g_heartbeat_domain, tbase, tsize, MEM_R | MEM_X);

    /* The device window: SIO_GPIO_OUT_SET (+0x18) and _CLR (+0x20) both
     * have to land in one region (mem_domain_permits()/PMP both require a
     * single access to resolve against exactly one region). Found on real
     * hardware, not predicted: an initial 64-byte region (the minimum that
     * spans both registers) read back from pmpaddr2 with its low bits
     * *not* matching what was written (0x...04 read back where 0x...07 was
     * written) -- every other NAPOT region in this tree is page-sized or
     * larger, so a region this small was untested territory. That
     * mismatch turned out to be a red herring (mem_domain_activate()'s own
     * success check already masks those low bits out, and widening to a
     * full page reproduced the identical masked-benign pattern); the
     * actual reason the GPIO writes had no effect despite no fault was
     * ACCESSCTRL_GPIO_NSMASK0 (see uart_init()'s comment on it), a filter
     * entirely upstream of and independent from PMP. Left at a full page
     * anyway: the same size class every other region in this codebase
     * (stacks, .utext, user program segments) already uses successfully,
     * "coarse but honest" rather than maximally narrow but unverified --
     * board_text_region() already grants a whole page for far less code
     * than it covers, for the same reason. */
    mem_domain_add(&g_heartbeat_domain, SIO_BASE, 4096, MEM_R | MEM_W);

    if (task_set_domain(sched_current_pid(), &g_heartbeat_domain) != 0) {
        /* Same honesty rule kernel/shell.c's user_task_common() follows:
         * a region the hardware did not install exactly as asked is not a
         * smaller grant, it is an unverified one, so refuse rather than
         * enter U-mode claiming isolation nothing confirmed. */
        printk("[Heartbeat] Refusing to enter U-mode: memory domain not enforceable\n");
        return;
    }
    /* ra = 0: heartbeat_umode_body() never returns (see its own comment) --
     * only the ELF loader's one-shot programs need a real return address. */
    arch_enter_user(heartbeat_umode_body,
                    (uintptr_t)g_heartbeat_ustack + sizeof(g_heartbeat_ustack),
                    0, 0, 0);
}

/* Called from kernel/main.c, after sched_init() -- unlike uart_init(),
 * which brings the hardware (including this LED's GPIO mux/OE bits) up
 * long before a task table exists. Not fatal if it fails: the board just
 * loses its visual heartbeat, the same as if this milestone had not
 * landed yet. */
int heartbeat_task_start(void) {
    int pid = task_create_driver("heartbeat", heartbeat_task_body, NULL, 1);
    if (pid < 0) return -1;
    task_set_priority(pid, TASK_PRIO_NORMAL);
    return pid;
}

/* M5 phase 1's own "Verify" deliverable: does the real heartbeat domain
 * shape (stack + .utext + a 4096-byte SIO window) actually confine the
 * task to GPIO, or does the SIO grant's width accidentally cover more?
 * Modeled directly on kernel/shell.c's user_intruder()/g_kernel_canary --
 * same idea (a deliberate out-of-domain store, asserted to fault), a
 * separate canary rather than reaching into shell.c's static one for the
 * same reason heartbeat_usys_yield()/_time_ms() are hand-rolled per file:
 * a HEARTBEAT_UATTR probe should not depend on anything outside this
 * translation unit. */
static volatile uintptr_t g_heartbeat_canary = 0xC0FFEE;

HEARTBEAT_UATTR static void heartbeat_intruder(void) {
    g_heartbeat_canary = 0xDEAD;
    /* Only reached if the store was NOT stopped -- if it faults (the
     * expected outcome) the task is torn down mid-store and never gets
     * here. Nothing to report through: the caller reads the canary
     * directly once the task is dead, matching cmd_usertest_isolation()'s
     * own "no escape signal needed" shape. */
    for (;;) { }
}

/* Allocated on demand for the duration of the test, not a permanent static
 * array: this is a diagnostic probe, run rarely and never concurrently with
 * itself, so it should cost nothing while idle -- the same reasoning
 * test_heap_on_demand (tests/hw/test_rp2350.py) exists to hold cc/ed to.
 * Found on real hardware, not predicted: a static 4096-byte array here was
 * tried first and quietly cost one whole extra heap page permanently (every
 * board's Image (data+bss) grows by exactly one page-worth of .bss the
 * moment this file links in), which was enough on its own to tip
 * test_heap_on_demand from passing to failing before any test had run at
 * all. palloc_pages() already returns page-aligned memory (page size ==
 * region granularity here), so no extra alignment attribute is needed. */

/* Set only once the task has actually reached U-mode -- without it, a
 * domain that failed to install would read identically to "the write was
 * correctly blocked" (same false-positive risk cmd_usertest_isolation()'s
 * own g_user_entered exists to rule out). */
static volatile bool g_heartbeat_intruder_entered;

static void heartbeat_intruder_task_body(void *arg) {
    uint8_t *ustack = (uint8_t *)arg;
    mem_domain_t dom;
    mem_domain_init(&dom);
    mem_domain_add(&dom, (uintptr_t)ustack, 4096, MEM_R | MEM_W);
    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&dom, tbase, tsize, MEM_R | MEM_X);
    /* The exact grant real heartbeat runs under -- this is what's on trial,
     * not a hypothetical wider one. */
    mem_domain_add(&dom, SIO_BASE, 4096, MEM_R | MEM_W);

    if (task_set_domain(sched_current_pid(), &dom) != 0) {
        printk("[HeartbeatIso] Refusing to enter U-mode: memory domain not enforceable\n");
        return;
    }
    g_heartbeat_intruder_entered = true;
    arch_enter_user(heartbeat_intruder, (uintptr_t)ustack + 4096, 0, 0, 0);
}

/* Runs the probe to completion and reports what actually happened. Returns
 * false if the task never reached U-mode (domain not enforceable on this
 * build/core, or the one-page stack could not be allocated) -- in that case
 * *out_canary and *out_exited_clean say nothing about isolation, matching
 * cmd_usertest_isolation()'s own "INCONCLUSIVE" case. */
bool heartbeat_isolation_test(uintptr_t *out_canary, bool *out_exited_clean) {
    g_heartbeat_canary = 0xC0FFEE;
    g_heartbeat_intruder_entered = false;

    void *ustack = palloc_pages(1);
    if (!ustack) {
        *out_canary = g_heartbeat_canary;
        *out_exited_clean = true;
        return false;
    }

    int pid = task_create("heartbeat_intruder", heartbeat_intruder_task_body, ustack);
    if (pid < 0) {
        palloc_free(ustack, 1);
        *out_canary = g_heartbeat_canary;
        *out_exited_clean = true;
        return false;
    }
    for (int i = 0; i < 10000 && sched_task_state(pid) != TASK_DEAD; i++) {
        sched_yield();
    }
    long status;
    *out_exited_clean = sched_task_exited_cleanly(pid, &status);
    palloc_free(ustack, 1);
    *out_canary = g_heartbeat_canary;
    return g_heartbeat_intruder_entered;
}

// Raw physical-UART byte access (defined below, after uart_putc()); handed
// to drivers/uart_net.c's A3b demux here in uart_init() so it needs a
// forward declaration.
static bool hw_uart_has_char(void);
static uint8_t hw_uart_getc(void);

/* M2, revised M2.5 (plan/phase12_microkernel_migration.md): PL011 UART0's
 * own IRQ line (RP2350 datasheet §2.1 IRQ table, confirmed against the Pico
 * SDK's hardware/regs/intctrl.h). Wired up in M2, unwired the same
 * milestone after hardware testing showed it corrupting console output, and
 * back now that kernel/printk.c's printk_lock()/printk_unlock() make a
 * whole message atomic across a block rather than just across a spin -- see
 * that file's comment for the full story.
 *
 * TX only, still deliberately: uart_getc()/uart_has_char() below poll TWO
 * independent sources every iteration -- the physical UART and USB CDC
 * (itself software-polled, not interrupt-driven) -- and this tree has no
 * "block with timeout" or "wait on any of N sources" primitive yet. That
 * reasoning is unrelated to the printk fix above and unchanged by it: RX
 * stays exactly as it was until a future milestone gives USB CDC its own
 * interrupt path or this tree gains a real multi-wait primitive. (The
 * heartbeat LED used to be a third thing stepped from this same poll --
 * M4.5, plan/phase12_microkernel_migration.md, Part B gave it its own
 * task instead, so it is no longer part of this reasoning.) */
#define UART0_IRQ 33u

/* At most one TX waiter: every caller now arrives through printk_lock()
 * (kernel/printk.c) already serialized to a single writer, so a second,
 * concurrent blocker here should not happen -- kept as a defensive
 * fallback below, not a load-bearing path, the same shape as
 * drivers/uart_16550.c's equivalent. */
static volatile int g_tx_waiter = -1;

#define UART0_FR    (UART0_BASE + 0x18)
#define UART0_IMSC  (UART0_BASE + 0x38)
#define UART0_FR_TXFF (1u << 5)
#define UART0_IMSC_TXIM (1u << 5)

static void uart_isr(void *ctx) {
    (void)ctx;
    if (!(REG(UART0_FR) & UART0_FR_TXFF) && g_tx_waiter >= 0) {
        REG(UART0_IMSC) &= ~UART0_IMSC_TXIM;
        int pid = g_tx_waiter;
        g_tx_waiter = -1;
        task_unblock(pid);
    }
}

void uart_init(uintptr_t base_addr) {
    (void)base_addr;

    /* 1. Explicitly enable clk_peri and attach it to clk_sys (150 MHz) */
    REG(CLOCKS_BASE + 0x48) = (1u << 11);

    /* 2. Unreset IO_BANK0 (bit 6), PADS_BANK0 (bit 9), UART0 (bit 26) */
    uint32_t unreset_mask = (1u << 6) | (1u << 9) | (1u << 26);
    REG(RESETS_ATOMIC_CLEAR) = unreset_mask;
    
    /* Wait for RESET_DONE status register @ 0x40020008 */
    while ((REG(RESETS_RESET_DONE) & unreset_mask) != unreset_mask);

    /* 3. Configure the heartbeat LED pin(s) for SIO (Function 5). The onboard
     *    one exists only on boards that have one -- see LED_MASK above. */
#ifdef CONFIG_LED_ONBOARD_GPIO
    REG(IO_BANK0_CTRL(CONFIG_LED_ONBOARD_GPIO)) = 5;
    REG(PADS_BANK0_PAD(CONFIG_LED_ONBOARD_GPIO)) = 0x56;
#endif
    REG(IO_BANK0_CTRL(CONFIG_LED_EXT_GPIO)) = 5;
    REG(PADS_BANK0_PAD(CONFIG_LED_EXT_GPIO)) = 0x56;
    REG(SIO_GPIO_OE_SET) = LED_MASK;

    /* M5, plan/phase12_microkernel_migration.md: GP16 specifically (not the
     * onboard LED, which only ever runs from kernel/M-mode code) needs to
     * be Non-secure-accessible, since heartbeat_task_body() below drives it
     * from real U-mode. Must happen here, from M-mode -- see
     * ACCESSCTRL_GPIO_NSMASK0's own comment above for why a U-mode task's
     * PMP domain cannot grant its way around this on its own. GPIO_NSMASK0
     * is one of the two ACCESSCTRL registers exempt from the 0xacce
     * write-protection prefix every other ACCESSCTRL register needs. */
    REG(ACCESSCTRL_GPIO_NSMASK0) |= (1u << CONFIG_LED_EXT_GPIO);

    /* M5 Phase 6: UART0 itself needs the same Non-secure grant, for the
     * "uart" task's own U-mode serve loop further below -- see
     * ACCESSCTRL_UART0's own comment above. */
    REG(ACCESSCTRL_UART0) = ACCESSCTRL_WRITE_PASSWORD | REG(ACCESSCTRL_UART0)
                            | ACCESSCTRL_UART0_NSP | ACCESSCTRL_UART0_NSU;

    /* Default GP16 to LOW (OFF in active-high configuration) */
    REG(SIO_GPIO_OUT_CLR) = (1u << CONFIG_LED_EXT_GPIO);

    /* 4. Mux GP0 to UART0 TX (Function 2), GP1 to UART0 RX (Function 2) */
    REG(IO_BANK0_CTRL(CONFIG_UART0_TX_GPIO)) = 2;
    REG(PADS_BANK0_PAD(CONFIG_UART0_TX_GPIO)) = 0x56;

    REG(IO_BANK0_CTRL(CONFIG_UART0_RX_GPIO)) = 2;
    REG(PADS_BANK0_PAD(CONFIG_UART0_RX_GPIO)) = 0x56;

    /* 5. Disable UART before programming baud rate and line control */
    REG(UART0_BASE + 0x30) = 0;

    /* 6. Configure Baud Rate for 150MHz clk_peri -> 115200 baud */
    REG(UART0_BASE + 0x24) = 81;  // UARTIBRD
    REG(UART0_BASE + 0x28) = 24;  // UARTFBRD

    /* 7. 8 bits, no parity, 1 stop bit, enable FIFOs */
    REG(UART0_BASE + 0x2C) = (3u << 5) | (1u << 4); // UARTLCR_H

    /* 8. Enable UART0, Transmit (TXE) & Receive (RXE) */
    REG(UART0_BASE + 0x30) = (1u << 0) | (1u << 8) | (1u << 9); // UARTCR

    uart_demux_init(hw_uart_has_char, hw_uart_getc);

    /* Handler before enable (arch/trap.h): so that if the IRQ somehow fires
     * before this returns, there is a handler to find it rather than an
     * unmasked source with nothing behind it. */
    devirq_attach(UART0_IRQ, uart_isr, NULL);
    arch_irq_enable(UART0_IRQ);

    uart_puts("\r\n[RP2350 Hardware UART0 Online] LugalOS Microkernel Starting...\r\n");
}

/* M2.5, plan/phase12_microkernel_migration.md: safe now that every caller
 * arrives through printk_lock() (kernel/printk.c) or is otherwise the sole
 * writer -- see that file's comment for the full story of what M2's first
 * attempt at this got wrong (real hardware caught `[Sche5d]` where `[Sched]`
 * should have been -- interleaved output, not a PMP failure, despite the
 * symptom initially reading like one). Same continuous-irq_save()
 * requirement as drivers/uart_16550.c's uart_putc(): nothing between the
 * fast-path miss and task_block() can restore interrupts early, or a TX
 * interrupt landing in the gap finds this task still RUNNING (not yet
 * BLOCKED), task_unblock() no-ops, and the ISR has already disabled TXIM
 * having "served" a wakeup nobody was asleep for -- a silent, permanent
 * lost wakeup. */
static void uart_hw_putc(char c) {
    if (!(REG(UART0_FR) & UART0_FR_TXFF)) {
        REG(UART0_BASE + 0x00) = (uint8_t)c;
        return;
    }

    /* The FIFO is full. Everything below assumes a scheduler: it registers
     * this task as the TX waiter, enables the TX interrupt and blocks. During
     * boot none of that exists -- there is no task to block and no handler to
     * wake it -- so task_block() there is simply a stop, and the machine ends
     * its life inside a printk.
     *
     * Which is exactly what happened (user, 2026-08-23). `[Dev] Registry: ...`
     * is around 60 characters and runs from dev_probe_all(), well before
     * sched_init(): the first 32 bytes filled the FIFO and byte 33 walked into
     * task_block() and never came out. It was the first boot-time message long
     * enough to overflow 32 bytes, which is why nothing earlier ever tripped
     * over it, and it looked maddeningly like a USB or power-source problem
     * because whether the FIFO had drained in time depended on how much delay
     * happened to precede it -- the boot beacon's own clicking was enough to
     * hide it completely.
     *
     * Before the scheduler, spin instead, and bound the spin: 50 ms is fifteen
     * times what 32 bytes need at 115200 baud, so it costs nothing in the
     * normal case, and a UART that is unclocked or misconfigured drops the
     * character rather than taking the kernel down with it. Losing a byte of
     * boot log is a bad outcome; not booting is a worse one. */
    if (!sched_is_active()) {
        uint64_t deadline = time_get_ms() + 50u;
        while ((REG(UART0_FR) & UART0_FR_TXFF) && time_get_ms() < deadline) { }
        if (REG(UART0_FR) & UART0_FR_TXFF) return;   /* wire is dead; drop it */
        REG(UART0_BASE + 0x00) = (uint8_t)c;
        return;
    }

    uintptr_t flags = irq_save();
    if (REG(UART0_FR) & UART0_FR_TXFF) {
        if (g_tx_waiter < 0) {
            g_tx_waiter = sched_current_pid();
            REG(UART0_IMSC) |= UART0_IMSC_TXIM;
            task_block();
        } else {
            irq_restore(flags);
            while (REG(UART0_FR) & UART0_FR_TXFF) sched_yield();
            flags = irq_save();
        }
    }
    irq_restore(flags);
    REG(UART0_BASE + 0x00) = (uint8_t)c;
}

// Raw physical-UART byte access (forward-declared above uart_init(), which
// hands these to drivers/uart_net.c's A3b demux) -- kept separate from
// uart_has_char()/uart_getc() below so the demux (once a user opts in via
// `p9share`) can pull straight from the PL011 registers without going
// through the console-ring indirection it is itself responsible for
// providing.
static bool hw_uart_has_char(void) {
    return (REG(UART0_BASE + 0x18) & (1u << 4)) == 0; // physical UART RX FIFO not empty
}

static uint8_t hw_uart_getc(void) {
    return (uint8_t)(REG(UART0_BASE + 0x00) & 0xFF);
}

/* --- M4.5/M5, plan/phase12_microkernel_migration.md: the uart task ---
 *
 * M5 Phase 6 narrowed this task's own duty to physical UART0 only -- see
 * this milestone's own plan notes ("split the duty across the privilege
 * boundary") for the full reasoning. The USB CDC mirror/read that used to
 * happen server-side now happens in the client facades below
 * (uart_flush()/uart_has_char()/uart_getc()), which already run in
 * ordinary kernel mode: usb_cdc.c is a *different* driver's hardware,
 * already exclusively owned by its own "usbcdc" task, and a U-mode uart
 * task cannot call into usb_cdc.c's kernel .text at all.
 *
 *   'H'      -> has-char query, physical UART0 only, non-consuming.
 *               resp: 1 byte, 0 or 1.
 *   'R'      -> read-if-ready, physical UART0 only, non-blocking (unlike
 *               the pre-M5-Phase-6 version, which blocked here for as
 *               long as a human took to press a key -- U-mode code
 *               cannot task_block()/irq_save(), so waiting now happens
 *               in the client's own loop instead, see uart_getc()
 *               below). resp: 2 bytes -- status (1 if a char was read, 0
 *               if none was ready), char (valid only if status is 1).
 *   'W', ... -> write the req_len-1 bytes following the opcode to
 *               physical UART0 only (no USB mirror -- see above).
 *               resp: empty.
 *
 * Deliberately NOT reachable when the A3b demux is enabled (`p9share`): the
 * facade functions below fall straight to direct hardware access in that
 * case, same as uart_16550.c, since the demux needs unmediated register
 * access to split 9P frame bytes from console bytes and is not itself
 * IPC-aware. */
#define UART_REQ_HASCHAR ((uint8_t)'H')
#define UART_REQ_READ    ((uint8_t)'R')
#define UART_REQ_WRITE   ((uint8_t)'W')

/* Must match (or exceed) uart_putc()'s UART_TX_BATCH_CAP, plus the opcode
 * byte -- the largest single 'W' request the endpoint can accept. */
#define UART_TX_BATCH_CAP 256
#define UART_REQ_CAP (UART_TX_BATCH_CAP + 1)
#define UART_RESP_CAP 2 /* 'R''s (status, char) -- the widest reply */

static uint8_t         g_uart_req[UART_REQ_CAP];
static uint8_t         g_uart_resp[UART_RESP_CAP];
static chan_endpoint_t *g_uart_ep;
static int              g_uart_task_pid = -1;

/* M4.5 verify: counts UART_REQ_WRITE calls actually served -- one per
 * uart_flush() that reached the task, not per character -- see
 * drivers/uart_16550.c's g_uart_write_calls comment for the reasoning.
 * M5 Phase 6: counted client-side now (uart_flush() below), matching
 * every other U-mode driver's own g_*_calls -- a U-mode server cannot
 * touch this ordinary kernel .bss global. */
static uint32_t g_uart_write_calls;

uint32_t uart_write_call_count(void) { return g_uart_write_calls; }

static bool uart_task_alive(void) {
    if (g_uart_task_pid < 0) return false;
    int st = sched_task_state(g_uart_task_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

/* ---- U-mode implementation, M5 Phase 6, plan/phase12_microkernel_migration.md ----
 *
 * A second, independent copy of the UART0 TX fast-path and RX peek/read,
 * tagged UART_UATTR and reachable only from the U-mode task's own serve
 * loop below -- not a refactor of uart_hw_putc()/hw_uart_has_char()/
 * hw_uart_getc(), which keep serving the direct-hardware fallback path
 * (including every boot-time uart_puts() call, which happens before this
 * task exists at all) exactly as before, unreachable from U-mode.
 *
 * TX has no ISR/task_block() equivalent here: irq_save()/task_block() are
 * M-mode-only operations U-mode cannot call. u_uart_putc() below busy-polls
 * TXFF and yields on a full FIFO instead -- the same "no interrupt
 * available here, so poll and yield" shape this file's own (pre-Phase-6)
 * RX loop, and drivers/uart_rp2350.c's own heartbeat_usleep_until(),
 * already used. uart_isr()/g_tx_waiter/UART0_IMSC are untouched and keep
 * serving uart_hw_putc()'s own block/wake path, used only when this task
 * is not alive or a WRITE gives up -- the two TX paths never run
 * concurrently, same "two independent copies" shape every prior driver's
 * domain-routed pair already has. */
#define UART_UATTR __attribute__((section(".utext"))) __attribute__((no_sanitize("undefined")))

/* Hand-rolled per translation unit -- see the syscall stubs further below
 * for why these aren't shared with any other file's own usys_*(). */
__attribute__((always_inline)) static inline long uart_usys_yield(void) {
    register long r_a0 __asm__("a0") = SYS_YIELD;
    __asm__ __volatile__("ecall" : "+r"(r_a0) :: "memory");
    return r_a0;
}

UART_UATTR static void u_uart_putc(char c) {
    while (REG(UART0_BASE + 0x18) & (1u << 5) /* TXFF */) {
        uart_usys_yield();
    }
    REG(UART0_BASE + 0x00) = (uint8_t)c;
}

UART_UATTR static bool u_uart_has_char(void) {
    return (REG(UART0_BASE + 0x18) & (1u << 4)) == 0; /* RX FIFO not empty */
}

UART_UATTR static uint8_t u_uart_getc(void) {
    return (uint8_t)(REG(UART0_BASE + 0x00) & 0xFF);
}

/* Hand-rolled per translation unit, not shared with any other driver's own
 * usys_*() stubs -- a UART_UATTR function must not call anything the
 * compiler might place outside .utext, and a cross-file inline is not a
 * guarantee. */
__attribute__((always_inline)) static inline long uart_usys_chan_serve_wait(const char *name, uint8_t *buf, long buf_max) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_WAIT;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = buf_max;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}
__attribute__((always_inline)) static inline long uart_usys_chan_serve_reply(const char *name, const uint8_t *buf, long len) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_REPLY;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = len;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}

UART_UATTR static void uart_umode_body(void) {
    /* Not a string literal: see drivers/tm1638_rp2350.c's own comment for
     * why (M5 Phase 2's first board-hanging bug) -- a literal lands in
     * ordinary .rodata, outside every region this task's domain grants.
     * volatile so gcc doesn't recognise the stores and reconstruct a
     * .rodata copy anyway. */
    volatile char name[5];
    name[0]='u'; name[1]='a'; name[2]='r'; name[3]='t'; name[4]='\0';

    /* Three cases, an if/else, never a switch -- see
     * drivers/st7735_rp2350.c's st7735_umode_body() comment for why
     * (jump tables outside the granted region); -fno-jump-tables is
     * applied to this file regardless, per that phase's own
     * "check preemptively" conclusion. */
    for (;;) {
        uint8_t req[UART_REQ_CAP];
        long req_len = uart_usys_chan_serve_wait((const char *)name, req, sizeof(req));
        if (req_len < 1) {
            uart_usys_chan_serve_reply((const char *)name, NULL, 0);
            continue;
        }

        uint8_t op = req[0];
        uint8_t resp[UART_RESP_CAP];
        if (op == UART_REQ_HASCHAR) {
            resp[0] = u_uart_has_char() ? 1 : 0;
            uart_usys_chan_serve_reply((const char *)name, resp, 1);
        } else if (op == UART_REQ_READ) {
            if (u_uart_has_char()) {
                resp[0] = 1;
                resp[1] = u_uart_getc();
            } else {
                resp[0] = 0;
            }
            uart_usys_chan_serve_reply((const char *)name, resp, 2);
        } else if (op == UART_REQ_WRITE) {
            for (long i = 1; i < req_len; i++) u_uart_putc((char)req[i]);
            uart_usys_chan_serve_reply((const char *)name, NULL, 0);
        } else {
            uart_usys_chan_serve_reply((const char *)name, NULL, 0);
        }
    }
}

/* M5 heap-reclaim, plan/phase12_microkernel_migration.md: 512 bytes, not
 * 4096 -- uart_umode_body() makes no calls post-inlining (measured
 * deepest stack use: 272 bytes). See g_heartbeat_ustack's own comment
 * above and .ustacks512's in linker/rp2350.ld. */
static uint8_t      g_uart_ustack[512] __attribute__((aligned(512)))
                                        __attribute__((section(".ustacks512")));
static mem_domain_t g_uart_domain;

/* This task's own kernel-mode entry point: task_create_sized() calls this
 * (ordinary kernel stack, kernel privilege) to build the domain and make
 * the one-way jump into U-mode. Simpler than st7735/blk's 4-region shape
 * (stack + text + SIO + a controller): uart's runtime hot path never
 * touches SIO (TX/RX pin muxing is uart_init()-time only, M-mode), so
 * this is the same 3-region shape tm1638/i2c already proved -- stack +
 * text + UART0_BASE. */
static void uart_task_body(void *arg) {
    (void)arg;
    while (!g_uart_ep) sched_yield();

    mem_domain_init(&g_uart_domain);
    mem_domain_add(&g_uart_domain, (uintptr_t)g_uart_ustack, sizeof(g_uart_ustack),
                   MEM_R | MEM_W);

    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&g_uart_domain, tbase, tsize, MEM_R | MEM_X);

    mem_domain_add(&g_uart_domain, UART0_BASE, 4096, MEM_R | MEM_W);

    if (task_set_domain(sched_current_pid(), &g_uart_domain) != 0) {
        printk("[UART] Refusing to enter U-mode: memory domain not enforceable; console stays on direct hardware access.\n");
        return;
    }
    arch_enter_user(uart_umode_body, (uintptr_t)g_uart_ustack + sizeof(g_uart_ustack), 0, 0, 0);
}

/* Called from kernel/main.c, after sched_init() -- unlike uart_init(),
 * which runs long before that and only brings the hardware itself up. Not
 * fatal if it fails: every facade function below falls back to direct
 * hardware access whenever uart_task_alive() is false. */
int uart_task_start(void) {
    int pid = task_create_driver("uart", uart_task_body, NULL, 1);
    if (pid < 0) {
        printk("[UART] Could not start the uart task; console stays on direct hardware access.\n");
        return -1;
    }
    /* TASK_PRIO_INTERRUPT: matches drivers/uart_16550.c's own uart task --
     * a caller that chan_call()s "uart" should not sit behind an arbitrary
     * NORMAL-tier queue for what is meant to be a near-instant handoff.
     * M5 Phase 6: every op this task now serves is small and bounded (no
     * more open-ended READ wait), so unlike the pre-Phase-6 version there
     * is no priority-drop dance needed inside the loop -- INTERRUPT tier
     * the whole time is simply correct now. */
    task_set_priority(pid, TASK_PRIO_INTERRUPT);
    if (chan_register_task("uart", pid, g_uart_req, sizeof(g_uart_req),
                           g_uart_resp, sizeof(g_uart_resp)) != 0) {
        printk("[UART] Could not register the uart channel endpoint; falling back to direct hardware access.\n");
        return -1;
    }
    g_uart_ep = chan_lookup("uart");
    g_uart_task_pid = pid;
    printk("[UART] Driver running as task #%d, reachable via chan_call(\"uart\", ...)\n", pid);
    return pid;
}

static int uart_call_with_retry(const uint8_t *req, uint32_t req_len,
                                uint8_t *resp, uint32_t resp_max) {
    for (int attempt = 0; attempt < 8; attempt++) {
        int n = chan_call(g_uart_ep, req, req_len, resp, resp_max);
        if (n >= 0) return n;
        sched_yield();
    }
    return -1;
}

/* M5 Phase 6's own "Verify" deliverable: does the real uart domain shape
 * (stack + .utext + a 4096-byte UART0_BASE window) actually confine the
 * task to UART0, or does the MMIO grant's width accidentally cover more?
 * Modeled directly on drivers/blk's/st7735's own isolation tests -- same
 * idea (a deliberate out-of-domain store, asserted to fault), a separate
 * canary rather than reaching into another file's, for the same reason
 * the syscall stubs above are hand-rolled per file. */
static volatile uintptr_t g_uart_canary = 0xC0FFEE;

UART_UATTR static void uart_intruder(void) {
    g_uart_canary = 0xDEAD;
    for (;;) { } /* only reached if the store was NOT stopped */
}

static volatile bool g_uart_intruder_entered;

/* `arg` is the U-mode stack -- allocated by uart_isolation_test() below,
 * not here, so it can free it once the task is confirmed DEAD (same
 * shape as every prior driver's own probe). */
static void uart_intruder_task_body(void *arg) {
    uint8_t *ustack = (uint8_t *)arg;
    mem_domain_t dom;
    mem_domain_init(&dom);
    mem_domain_add(&dom, (uintptr_t)ustack, 4096, MEM_R | MEM_W);
    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&dom, tbase, tsize, MEM_R | MEM_X);
    /* The exact grant real uart runs under -- this is what's on trial. */
    mem_domain_add(&dom, UART0_BASE, 4096, MEM_R | MEM_W);

    if (task_set_domain(sched_current_pid(), &dom) != 0) {
        printk("[UartIso] Refusing to enter U-mode: memory domain not enforceable\n");
        return;
    }
    g_uart_intruder_entered = true;
    arch_enter_user(uart_intruder, (uintptr_t)ustack + 4096, 0, 0, 0);
}

/* Runs the probe to completion and reports what actually happened. Returns
 * false if the task never reached U-mode (domain not enforceable on this
 * build/core, or the one-page stack could not be allocated) -- in that
 * case *out_canary and *out_exited_clean say nothing about isolation,
 * matching cmd_usertest_isolation()'s own "INCONCLUSIVE" case. */
bool uart_isolation_test(uintptr_t *out_canary, bool *out_exited_clean) {
    g_uart_canary = 0xC0FFEE;
    g_uart_intruder_entered = false;

    void *ustack = palloc_pages(1);
    if (!ustack) {
        *out_canary = g_uart_canary;
        *out_exited_clean = true;
        return false;
    }

    int pid = task_create("uart_intruder", uart_intruder_task_body, ustack);
    if (pid < 0) {
        palloc_free(ustack, 1);
        *out_canary = g_uart_canary;
        *out_exited_clean = true;
        return false;
    }
    for (int i = 0; i < 10000 && sched_task_state(pid) != TASK_DEAD; i++) {
        sched_yield();
    }
    long status;
    *out_exited_clean = sched_task_exited_cleanly(pid, &status);
    *out_canary = g_uart_canary;
    palloc_free(ustack, 1);
    return g_uart_intruder_entered;
}

/* M4.5: batches characters and sends one chan_call() per chunk (or per
 * uart_flush()) instead of one per character -- same reasoning and same
 * shape as drivers/uart_16550.c's own uart_putc()/uart_flush(), including
 * the while (not if) loop below (see that file's comment for why: without
 * it, a task that finds the batch full, releases the lock to flush, and
 * gets preempted before re-appending would let a second task refill the
 * just-flushed batch in the gap). */
static char     g_tx_batch[UART_TX_BATCH_CAP];
static uint32_t g_tx_batch_len;
/* Guards g_tx_batch/g_tx_batch_len (X2,
 * plan/phase23_multicore_scheduling.md).
 *
 * X2 pins every driver task to hart 0, and phase 22's S5 left the UART
 * paths unconverted on the strength of that. Checking the claim rather than
 * inheriting it -- which is what X2 is for -- shows it covers less than S5
 * assumed. It is true of the *waiter* path below: uart_hw_putc_blocking()
 * is reached only from the driver task itself, except on the fallback taken
 * when that task is absent or the p9share demux owns the wire.
 *
 * It is NOT true of this batch. uart_flush() and uart_putc() run in the
 * context of whoever is printing -- printk_unlock() calls the first, every
 * console write the second -- so on two harts two printing tasks touch this
 * buffer at once, and irq_save() never covered that. Same shape as
 * usb_cdc_putc(), and found the same way: by asking who actually calls it
 * rather than which file it lives in. */
static spinlock_t g_tx_batch_lock;

void uart_flush(void) {
    char local[UART_TX_BATCH_CAP];
    uintptr_t flags = spin_lock_irqsave(&g_tx_batch_lock);
    uint32_t len = g_tx_batch_len;
    if (len > 0) {
        memcpy(local, g_tx_batch, len);
        g_tx_batch_len = 0;
    }
    spin_unlock_irqrestore(&g_tx_batch_lock, flags);
    if (len == 0) return;

    if (uart_demux_is_enabled() || !uart_task_alive()) {
        for (uint32_t i = 0; i < len; i++) {
            uart_hw_putc(local[i]);
            usb_cdc_putc(local[i]);
        }
        return;
    }
    uint8_t req[1 + UART_TX_BATCH_CAP];
    req[0] = UART_REQ_WRITE;
    memcpy(&req[1], local, len);
    uint8_t resp[1];
    /* Not uart_call_with_retry()'s plain bounded retry: a WRITE must never
     * give up and fall back to direct hardware access while another
     * WRITE is actually in flight (a real race on real hardware, not a
     * theoretical one -- see the git history of this comment for the
     * exact interleaved-output symptom it was found by), while still
     * falling back promptly when the endpoint is merely busy with some
     * other, near-instant op instead.
     *
     * M5 Phase 6: chan_endpoint_busy(g_uart_ep) (kernel/chan.c) replaces
     * the old server-set g_uart_write_in_flight flag -- a U-mode server
     * cannot write an ordinary kernel .bss global, but chan_call()
     * itself already tracks exactly this window (ep->busy, true from
     * the moment a caller's request is copied in until that caller's
     * reply comes back), so this reads that directly instead of keeping
     * a second, redundant flag. Every op this task now serves is small
     * and bounded (no more open-ended READ), so "busy" here can only
     * ever mean "a few register pokes are in flight", never "someone is
     * waiting on a human keystroke". */
    for (;;) {
        int n = chan_call(g_uart_ep, req, 1 + len, resp, sizeof(resp));
        if (n >= 0) {
            g_uart_write_calls++;
            for (uint32_t i = 0; i < len; i++) usb_cdc_putc(local[i]);
            return;
        }
        if (!chan_endpoint_busy(g_uart_ep)) break;
        sched_yield();
    }
    for (uint32_t i = 0; i < len; i++) {
        uart_hw_putc(local[i]);
        usb_cdc_putc(local[i]);
    }
}

void uart_putc(char c) {
    uintptr_t flags = spin_lock_irqsave(&g_tx_batch_lock);
    while (g_tx_batch_len >= UART_TX_BATCH_CAP) {
        /* Released around uart_flush(), which takes this same lock --
         * spinlock_t is not re-entrant, and the re-check on re-acquire is
         * what makes dropping it safe (the existing comment above the batch
         * explains why the re-check was already required). */
        spin_unlock_irqrestore(&g_tx_batch_lock, flags);
        uart_flush();
        flags = spin_lock_irqsave(&g_tx_batch_lock);
    }
    g_tx_batch[g_tx_batch_len++] = c;
    spin_unlock_irqrestore(&g_tx_batch_lock, flags);
}

void uart_debug_putc(char c) {
    uart_hw_putc(c); // kernel debug log: physical UART only, no USB mirror, no batching
}

// The A3b demux only ever applies to the physical UART0 wire -- USB CDC
// ACM0 is a channel of its own with no shared-wire ambiguity, so it keeps
// being checked directly and unconditionally, same as before this
// conversion. Flushed first: an outstanding write (a prompt, most often)
// should be visible before anything checks for or waits on the reply it
// usually precedes -- same reasoning as drivers/uart_16550.c's equivalent.
/* M5 Phase 6: when uart_task_alive(), physical UART0 is checked through
 * chan_call() only -- see "this task, and only this task, may call
 * uart_hw_putc()/hw_uart_has_char()/hw_uart_getc() while alive" above;
 * hw_uart_has_char()/hw_uart_getc() are reached directly only when the
 * task is NOT alive, never as a same-caller fallback after a retry
 * budget runs out (the pre-Phase-6 version of both facades did exactly
 * that, a latent race against the task's own exclusive hardware access
 * -- harmless in practice since chan_call() only fails this way under
 * real contention, but incorrect on its own terms; fixed here rather
 * than carried forward while this function was already being rewritten
 * for the U-mode split). */
/* The console on this board is USB-CDC in practice (the physical UART has no
 * terminal on the clock persona), and the USB ring is the one that can be
 * scanned without consuming -- so that is where an interrupt is looked for.
 * The UART's own FIFO cannot be inspected non-destructively and is not
 * covered: reading it to look would be reading it. */
bool uart_peek_interrupt(void) {
    return usb_cdc_peek_interrupt();
}

bool uart_has_char(void) {
    uart_flush();
    /* M4.5: the "usbcdc" background task (drivers/usb_cdc.c) now services
     * this on its own schedule; only pump it directly here as a fallback
     * for the (untested-in-practice) case that task failed to start. */
    if (!usb_cdc_task_alive()) usb_cdc_task();
    if (uart_demux_is_enabled()) {
        if (uart_demux_console_has_char()) return true;
        return usb_cdc_has_char();
    }
    if (uart_task_alive()) {
        uint8_t req[1] = { UART_REQ_HASCHAR };
        uint8_t resp[1];
        if (uart_call_with_retry(req, 1, resp, 1) == 1 && resp[0]) return true;
    } else if (hw_uart_has_char()) {
        return true;
    }
    return usb_cdc_has_char();
}

char uart_getc(void) {
    uart_flush();
    if (uart_demux_is_enabled()) {
        while (!uart_demux_console_has_char()) {
            if (!usb_cdc_task_alive()) usb_cdc_task();
            sched_yield();
        }
        return uart_demux_console_getc();
    }
    /* M5 Phase 6: the wait that used to live server-side (blocking inside
     * the task's own READ handler for as long as a human took to press a
     * key) moves here -- U-mode code cannot task_block(), so the U-mode
     * READ op is now a single non-blocking check (see uart_umode_body()
     * above), and this loop polls it the same way it already polled
     * usb_cdc_has_char()/getc() before this phase. */
    for (;;) {
        if (!usb_cdc_task_alive()) usb_cdc_task();
        if (uart_task_alive()) {
            uint8_t req[1] = { UART_REQ_READ };
            uint8_t resp[2];
            int n = uart_call_with_retry(req, 1, resp, sizeof(resp));
            if (n >= 2 && resp[0]) return (char)resp[1];
        } else if (hw_uart_has_char()) {
            return (char)hw_uart_getc();
        }
        if (usb_cdc_has_char()) return usb_cdc_getc();
        sched_yield();
    }
}

void uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_debug_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_debug_putc('\r');
        uart_debug_putc(*s++);
    }
}
