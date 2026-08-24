#include "drivers/uart.h"
#include "drivers/uart_net.h"
#include "kernel/devirq.h"
#include "kernel/sched.h"
#include "kernel/irq.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include "arch/trap.h"
#include <string.h>

static volatile uint8_t *uart_base = (volatile uint8_t *)0x10000000; // QEMU virt UART0 base

/* 16550 Register Offsets */
#define UART_RBR 0 // Receive Buffer Register
#define UART_THR 0 // Transmit Holding Register
#define UART_IER 1 // Interrupt Enable Register
#define UART_LSR 5 // Line Status Register
#define UART_LSR_DR   0x01 // Data Ready
#define UART_LSR_THRE 0x20 // Transmit Holding Register Empty
#define UART_IER_ERBFI 0x01 // Enable Received Data Available interrupt
#define UART_IER_ETBEI 0x02 // Enable Transmitter Holding Register Empty interrupt

/* QEMU virt's ns16550, IRQ 10 -- verified against qemu/include/hw/riscv/virt.h
 * rather than assumed (M2, plan/phase12_microkernel_migration.md). */
#define UART0_IRQ 10u

/* M2, revised M2.5: the ISR-driven wake this file lacked entirely before --
 * both directions. See kernel/printk.c for why TX blocking is safe now.
 *
 * The 16550's RX/TX-ready conditions are level-triggered on FIFO fill
 * state, not edge/latched, so an interrupt source left enabled with nothing
 * waiting on it fires on every return from interrupt for as long as the
 * condition holds -- an interrupt storm, not a hang, but one that starves
 * every other task just as effectively. So IER's bits are enabled only for
 * the duration of an actual wait, by whichever task is about to block, and
 * disabled again by the ISR the moment it has served that one waiter.
 *
 * M4: these two slots are shared between the uart task's own service loop
 * (the common case, once it is up) and the direct-access fallback paths
 * below (bootstrap, `p9share`, or a dead uart task) -- both go through the
 * same uart_hw_*_blocking() primitives, so at most one waiter per direction
 * still holds regardless of who the caller is. A second, concurrent
 * blocking call in the same direction falls back to plain polling rather
 * than being silently queued -- the same "single-slot, busy refuses" shape
 * chan_call() itself uses for the same reason. */
static volatile int g_rx_waiter = -1;
static volatile int g_tx_waiter = -1;

static void uart_isr(void *ctx) {
    (void)ctx;
    uint8_t lsr = uart_base[UART_LSR];
    if ((lsr & UART_LSR_DR) && g_rx_waiter >= 0) {
        uart_base[UART_IER] &= (uint8_t)~UART_IER_ERBFI;
        int pid = g_rx_waiter;
        g_rx_waiter = -1;
        task_unblock(pid);
    }
    if ((lsr & UART_LSR_THRE) && g_tx_waiter >= 0) {
        uart_base[UART_IER] &= (uint8_t)~UART_IER_ETBEI;
        int pid = g_tx_waiter;
        g_tx_waiter = -1;
        task_unblock(pid);
    }
}

// Raw hardware byte access, handed to drivers/uart_net.c's A3b demux at
// init time. Kept separate from uart_has_char()/uart_getc() below so the
// demux (when a user opts into it via `p9share`) can pull straight from the
// hardware register without going through the console-ring indirection it
// is itself responsible for providing.
static bool hw_uart_has_char(void) {
    if (!uart_base) return false;
    return (uart_base[UART_LSR] & UART_LSR_DR) != 0;
}

static uint8_t hw_uart_getc(void) {
    return uart_base[UART_RBR];
}

/* The actual hardware wait/transfer, M4 (plan/phase12_microkernel_migration.md):
 * pulled out of uart_putc()/uart_getc() so both the uart task's own service
 * loop and the direct-access fallback paths share one implementation rather
 * than two copies of the same continuous-irq_save() logic drifting apart.
 * Whoever calls these currently owns the hardware -- normally the uart
 * task, exclusively, once it is up. */
static void uart_hw_putc_blocking(char c) {
    if (!uart_base) return;
    if ((uart_base[UART_LSR] & UART_LSR_THRE) != 0) {
        uart_base[UART_THR] = (uint8_t)c;
        return;
    }
    /* Same continuous-irq_save() requirement as uart_hw_getc_blocking()
     * below: nothing between the fast-path miss and task_block() can
     * restore interrupts early, or a TX interrupt landing in the gap finds
     * this task still RUNNING (not yet BLOCKED), task_unblock() no-ops, and
     * the ISR has already disabled ETBEI having "served" a wakeup nobody
     * was asleep for -- a silent, permanent lost wakeup. */
    uintptr_t flags = irq_save();
    if ((uart_base[UART_LSR] & UART_LSR_THRE) == 0) {
        if (g_tx_waiter < 0) {
            g_tx_waiter = sched_current_pid();
            uart_base[UART_IER] |= UART_IER_ETBEI;
            task_block();
        } else {
            irq_restore(flags);
            while ((uart_base[UART_LSR] & UART_LSR_THRE) == 0) sched_yield();
            flags = irq_save();
        }
    }
    irq_restore(flags);
    uart_base[UART_THR] = (uint8_t)c;
}

static uint8_t uart_hw_getc_blocking(void) {
    if (hw_uart_has_char()) return hw_uart_getc();
    uintptr_t flags = irq_save();
    if (!hw_uart_has_char()) {
        if (g_rx_waiter < 0) {
            g_rx_waiter = sched_current_pid();
            uart_base[UART_IER] |= UART_IER_ERBFI;
            task_block();
        } else {
            irq_restore(flags);
            while (!hw_uart_has_char()) sched_yield();
            flags = irq_save();
        }
    }
    irq_restore(flags);
    return hw_uart_getc();
}

/* --- M4: the uart driver task ---
 *
 * A minimal wire protocol over the "uart" task-owned channel
 * (kernel/chan.h). Single opcode byte, then a variable-length payload.
 *
 *   'H'      -> has-char query.  resp: 1 byte, 0 or 1.
 *   'R'      -> read (blocks internally, ISR-driven, if the FIFO is empty).
 *               resp: 1 byte, the char read.
 *   'W', ... -> write the req_len-1 bytes following the opcode (blocks
 *               internally, per byte, if the FIFO is full). resp: empty.
 *
 * 'W' carries a *buffer*, not a single byte, deliberately: the first version
 * of this milestone gave it exactly one payload byte, which meant every
 * character of console output was its own chan_call() -- a genuine
 * task_block()/task_unblock()/context-switch round trip on every byte any
 * task printed, turning a scheduling decision that used to be nearly free
 * into a several-times-per-line event. uart_putc() below batches into
 * UART_TX_BATCH_CAP-sized chunks and sends one 'W' per chunk instead, which
 * is what actually fixes that -- not anything about this task or the
 * endpoint mechanism, both of which were already correct. See
 * uart_putc()/uart_flush()'s own comments for the batching half.
 *
 * g_uart_req/g_uart_resp are the SAME buffers handed to
 * chan_register_task() below -- chan_call() copies a caller's request
 * straight into them, so the task reads its own request out of the same
 * static array it registered, no accessor function needed even though
 * chan_endpoint_t itself is opaque outside kernel/chan.c. */
#define UART_REQ_HASCHAR ((uint8_t)'H')
#define UART_REQ_READ    ((uint8_t)'R')
#define UART_REQ_WRITE   ((uint8_t)'W')

/* Must match (or exceed) uart_putc()'s UART_TX_BATCH_CAP, plus the opcode
 * byte -- this is the largest single 'W' request the endpoint can accept. */
#define UART_REQ_CAP (256 + 1)

static uint8_t         g_uart_req[UART_REQ_CAP];
static uint8_t         g_uart_resp[1];
static chan_endpoint_t *g_uart_ep;
static int              g_uart_task_pid = -1;

/* M4 verify: counts UART_REQ_WRITE calls actually served -- one per
 * uart_flush() that reached the task (not per character, and not per
 * uart_putc()), so a test can check IPC volume tracks *lines/messages*
 * instead of guessing from timing. See uart_write_call_count(). */
static uint32_t g_uart_write_calls;

uint32_t uart_write_call_count(void) { return g_uart_write_calls; }

/* M4.5, found while bringing up drivers/uart_rp2350.c's own uart task on
 * real hardware -- see that file's own comment on this same flag for the
 * full account. True only while UART_REQ_WRITE below is actually pushing
 * bytes to the hardware, false at every other time (including the whole
 * duration of a pending READ, which can legitimately run for as long as a
 * human takes to press a key). uart_flush()'s retry loop uses this to
 * distinguish "busy with a bounded WRITE -- wait it out, never race it"
 * from "busy with an unbounded READ -- fall back to direct access right
 * away" (READ never touches the TX-side hardware this WRITE handler does,
 * so a concurrent direct-access WRITE cannot collide with it).
 *
 * Without this distinction, a second WRITE that outlasts a plain bounded
 * retry can fall back to direct hardware access *while the task is still
 * transmitting the first one* -- on QEMU's near-instant emulated 16550
 * this essentially never manifests (retries essentially never exhaust),
 * but it is the exact same defect that produced reliably reproducible,
 * byte-level interleaved console garbage on real RP2350 hardware, where a
 * full batch takes real, non-negligible time to transmit. Fixed here too
 * rather than left as a latent, harder-to-trigger version of the same bug. */
static volatile bool g_uart_write_in_flight;

static bool uart_task_alive(void) {
    if (g_uart_task_pid < 0) return false;
    int st = sched_task_state(g_uart_task_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

/* This task, and only this task, may call uart_hw_putc_blocking()/
 * uart_hw_getc_blocking() while alive -- everything else reaches the
 * hardware through chan_call("uart", ...) via the facade functions below.
 * Must never call uart_putc()/uart_getc()/uart_has_char() itself: that
 * would chan_call() into its own endpoint, which chan_call()'s busy flag
 * correctly refuses rather than deadlocks, but there is no reason to rely
 * on that -- it should simply never happen. */
/* Must never call printk()/printk_debug()/cprintf() (or anything that takes
 * printk_lock()) from within this loop -- chan.c's wait-for cycle guard
 * covers chan_call() itself, but printk_lock() is a *different* blocking
 * resource it cannot see. A caller can legitimately be blocked here on this
 * very endpoint while holding printk_lock(); this task taking that same
 * lock to log would deadlock against it. uart_debug_putc()/uart_debug_puts()
 * exist precisely so a driver can still get bytes out under that
 * constraint. */
static void uart_task_body(void *arg) {
    (void)arg;
    /* uart_task_start() creates this task before the endpoint it serves
     * exists (chan_register_task() needs the pid first) -- wait for
     * registration to finish rather than risk running before g_uart_ep is
     * set, which a preemption landing in that narrow window could do. */
    while (!g_uart_ep) sched_yield();

    for (;;) {
        uint32_t req_len = chan_serve_wait(g_uart_ep);
        if (req_len == 0) { chan_serve_reply(g_uart_ep, 0); continue; }
        switch (g_uart_req[0]) {
            case UART_REQ_HASCHAR:
                g_uart_resp[0] = hw_uart_has_char() ? 1 : 0;
                chan_serve_reply(g_uart_ep, 1);
                break;
            case UART_REQ_READ:
                g_uart_resp[0] = uart_hw_getc_blocking();
                chan_serve_reply(g_uart_ep, 1);
                break;
            case UART_REQ_WRITE:
                g_uart_write_calls++;
                g_uart_write_in_flight = true;
                for (uint32_t i = 1; i < req_len; i++) {
                    uart_hw_putc_blocking((char)g_uart_req[i]);
                }
                g_uart_write_in_flight = false;
                chan_serve_reply(g_uart_ep, 0);
                break;
            default:
                chan_serve_reply(g_uart_ep, 0);
                break;
        }
    }
}

/* Called from kernel/main.c, after sched_init() (task_create() needs a
 * table to create into) -- unlike uart_init() below, which runs long
 * before that and only brings the hardware itself up. Failure here is not
 * fatal: every caller's facade function falls back to direct hardware
 * access whenever uart_task_alive() is false, so a board that could not
 * start this task for some reason keeps working exactly as it did before
 * M4, just without the isolation the task provides. */
int uart_task_start(void) {
    int pid = task_create_sized("uart", uart_task_body, NULL, 1); /* M0: shallow call depth, 1 page is generous */
    if (pid < 0) {
        printk("[UART] Could not start the uart task; console stays on direct hardware access.\n");
        return -1;
    }
    /* TASK_PRIO_INTERRUPT: a caller that chan_call()s "uart" unblocks this
     * task, then blocks itself (kernel/chan.c's chan_call_task()) -- at
     * TASK_PRIO_NORMAL like everything else, next_runnable() could pick any
     * other ready task first, sitting the caller behind an arbitrary queue
     * for what is meant to be a near-instant hardware handoff. sched.h
     * reserved this tier for exactly this role. */
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

void uart_init(uintptr_t base_addr) {
    if (base_addr != 0) {
        uart_base = (volatile uint8_t *)base_addr;
    }
    uart_demux_init(hw_uart_has_char, hw_uart_getc);
    /* Handler before enable (arch/trap.h): so that if the IRQ somehow fires
     * before this returns, there is a handler to find it rather than an
     * unmasked source with nothing behind it. */
    devirq_attach(UART0_IRQ, uart_isr, NULL);
    arch_irq_enable(UART0_IRQ);
}

/* Retries a bounded number of times on a transient "busy" (the endpoint's
 * single in-flight slot occupied by someone else) rather than either
 * blocking indefinitely or falling straight back to a hardware access that
 * would race the request already in flight. Busy is expected to be rare:
 * this driver's writers are already serialized by uart_putc()'s own batch
 * lock (see below), and it has exactly one reader in practice. Exhausting
 * the retries is the only case that falls through to direct access --
 * unlike a dead or not-yet-started task, this is a real, if unlikely,
 * degradation, not the common path. */
static int uart_call_with_retry(const uint8_t *req, uint32_t req_len,
                                uint8_t *resp, uint32_t resp_max) {
    for (int attempt = 0; attempt < 8; attempt++) {
        int n = chan_call(g_uart_ep, req, req_len, resp, resp_max);
        if (n >= 0) return n;
        sched_yield();
    }
    return -1;
}

/* M4: batches characters into a chunk and sends one chan_call() per chunk
 * (or per uart_flush()) instead of one per character. This -- not the
 * task/endpoint mechanism, which was already correct -- is what fixes the
 * first attempt's actual defect: every printk()/cprintf() call formats
 * character-by-character through this function (kernel/printk.c's
 * vprintk_to() calls its `pc` callback once per literal char, per digit,
 * per padding space...), so leaving it un-batched would still turn one log
 * line into a dozen-plus IPC round trips even with everything else fixed.
 *
 * g_tx_batch is shared, mutable state now reachable from any task that
 * prints -- console_putc()/console_puts() (line_editor.c's redraws,
 * SYS_PUTCHAR) call this directly, without going through printk_lock(), so
 * that lock cannot be assumed to already serialize access here. Protected
 * with irq_save() instead: short, bounded, never itself blocks (the actual
 * I/O -- chan_call() or the hardware loop -- always happens after
 * irq_restore(), on a local copy, exactly like uart_hw_putc_blocking()'s
 * own fast-path/slow-path split above). The while loop in uart_putc()
 * (not an if) matters: without it, a task that finds the batch already
 * full, releases the mask to flush, and gets preempted before it can
 * re-append would let a second task fill the (now-empty, post-flush)
 * batch again in the gap, and the first task's unconditional append after
 * a single flush attempt would overrun it. Re-checking under the lock each
 * time closes that. */
#define UART_TX_BATCH_CAP 256
static char     g_tx_batch[UART_TX_BATCH_CAP];
static uint32_t g_tx_batch_len;

void uart_flush(void) {
    char local[UART_TX_BATCH_CAP];
    uintptr_t flags = irq_save();
    uint32_t len = g_tx_batch_len;
    if (len > 0) {
        memcpy(local, g_tx_batch, len);
        g_tx_batch_len = 0;
    }
    irq_restore(flags);
    if (len == 0) return;

    if (uart_demux_is_enabled() || !uart_task_alive()) {
        for (uint32_t i = 0; i < len; i++) uart_hw_putc_blocking(local[i]);
        return;
    }
    uint8_t req[1 + UART_TX_BATCH_CAP];
    req[0] = UART_REQ_WRITE;
    memcpy(&req[1], local, len);
    uint8_t resp[1];
    /* Not uart_call_with_retry()'s plain bounded retry: see
     * g_uart_write_in_flight's own comment for why a WRITE specifically
     * must never give up and fall back to direct access while another
     * WRITE is actually in flight, while still falling back promptly --
     * not waiting out a human's keystroke -- when the endpoint is merely
     * busy with a pending READ instead. */
    for (;;) {
        int n = chan_call(g_uart_ep, req, 1 + len, resp, sizeof(resp));
        if (n >= 0) return;
        if (!g_uart_write_in_flight) break;
        sched_yield();
    }
    for (uint32_t i = 0; i < len; i++) uart_hw_putc_blocking(local[i]);
}

void uart_putc(char c) {
    uintptr_t flags = irq_save();
    while (g_tx_batch_len >= UART_TX_BATCH_CAP) {
        irq_restore(flags);
        uart_flush();
        flags = irq_save();
    }
    g_tx_batch[g_tx_batch_len++] = c;
    irq_restore(flags);
}

// When the A3b demux is enabled (opt-in, via `p9share`), the console can no
// longer read the hardware register directly -- some of what's waiting
// there may be 9P frame bytes. uart_demux_console_has_char() pumps the
// hardware and returns only the console-routed subset; disabled (the
// default), this now asks the uart task the same question M2.5 asked the
// register directly (see uart_flush()'s comment for why "task not alive"
// falls back rather than failing). Flushed first: an outstanding write
// (a prompt, most often) should be visible before anything checks for or
// waits on the reply it usually precedes.
/* QEMU's console is the 16550's own FIFO, which cannot be looked at without
 * taking the byte -- so there is nothing to peek and the pump keeps its
 * pre-existing behaviour on this target. */
bool uart_peek_interrupt(void) {
    return false;
}

bool uart_has_char(void) {
    uart_flush();
    if (uart_demux_is_enabled()) return uart_demux_console_has_char();
    if (uart_task_alive()) {
        uint8_t req[1] = { UART_REQ_HASCHAR };
        uint8_t resp[1];
        if (uart_call_with_retry(req, 1, resp, 1) == 1) return resp[0] != 0;
    }
    return hw_uart_has_char();
}

char uart_getc(void) {
    uart_flush();
    if (uart_demux_is_enabled()) {
        while (!uart_demux_console_has_char()) sched_yield();
        return uart_demux_console_getc();
    }
    if (uart_task_alive()) {
        uint8_t req[1] = { UART_REQ_READ };
        uint8_t resp[1];
        if (uart_call_with_retry(req, 1, resp, 1) == 1) return (char)resp[0];
    }
    return (char)uart_hw_getc_blocking();
}

void uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

// Low-level driver diagnostics only (see kernel/printk.h's printk_debug()):
// deliberately never routed through the uart task or the batching buffer
// above, so tracing that might be used to debug either mechanism does not
// depend on them, and never interleaves with a batch some other task is
// mid-way through accumulating.
void uart_debug_putc(char c) {
    uart_hw_putc_blocking(c);
}

void uart_debug_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_debug_putc('\r');
        uart_debug_putc(*s++);
    }
}
