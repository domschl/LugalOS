#include "arch/atomic.h"
#include "drivers/uart.h"
#include "drivers/uart_proto.h"
#include "drivers/uart_net.h"
#include "drivers/driver_task.h"
#include "kernel/devirq.h"
#include "kernel/sched.h"
#include "kernel/hart.h"
#include "kernel/irq.h"
#include "kernel/lock.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include "arch/trap.h"
#include <string.h>

/* single task: the boot (uart_init), before the line is enabled. */
static volatile uint8_t *uart_base = (volatile uint8_t *)0x10000000; // QEMU virt UART0 base

/* 16550 Register Offsets */
#define UART_RBR 0 // Receive Buffer Register
#define UART_THR 0 // Transmit Holding Register
#define UART_IER 1 // Interrupt Enable Register
#define UART_LSR 5 // Line Status Register
#define UART_LSR_DR   0x01 // Data Ready
/* Overrun Error: a character arrived before the previous one was read, and
 * the previous one is gone. The 16550 latches this in LSR and clears it on
 * read, so it is free to notice and invisible if nobody looks -- and nobody
 * did until 2026-09-13, when a suite run sent `identity provision` and the
 * shell never echoed it at all. A lost *echo* is not an output race; it means
 * the input never arrived, and this is the bit that says so. */
#define UART_LSR_OE   0x02 // Overrun Error -- a received byte was dropped

/* Every received byte the hardware dropped because nothing read the previous
 * one in time. See UART_LSR_OE. */
/* accepted: a statistic, counted by the handler and by the polling read; a
 * lost increment is a miscount by one, and the first message may repeat. */
static volatile uint32_t g_uart_rx_overruns;
uint32_t uart_rx_overruns(void) { return g_uart_rx_overruns; }

/* Says so the first time, rather than waiting to be asked.
 *
 * A dropped input byte is invisible by construction: the shell never sees the
 * character, so it never echoes it and never acts on it, and the only trace is
 * a command that appears not to have been sent. Polling a counter cannot catch
 * that -- by the time a test thinks to look, the run it would explain is over.
 *
 * printk() and not printk_critical(): from a device handler the former is a
 * ring append and a wake, and reaches no blocking primitive (klog_emit()
 * checks devirq_in_handler() before its boot-time inline drain), which is
 * what makes it safe from the interrupt handler below. printk_critical() would reach
 * uart_flush_critical() and the batch spinlock -- the mistake that hung three
 * suite runs earlier in this investigation. */
static void note_rx_overrun(void) {
    if (g_uart_rx_overruns++ == 0) {
        printk("[UART] RX OVERRUN: a received byte was dropped; input was "
               "lost, not delayed\n");
    }
}
#define UART_LSR_THRE 0x20 // Transmit Holding Register Empty
#define UART_IER_ERBFI 0x01 // Enable Received Data Available interrupt
#define UART_IER_ETBEI 0x02 // Enable Transmitter Holding Register Empty interrupt

/* QEMU virt's ns16550, IRQ 10 -- verified against qemu/include/hw/riscv/virt.h
 * rather than assumed. */
#define UART0_IRQ 10u

/* the ISR-driven wake this file lacked entirely before --
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
 * these two slots are shared between the uart task's own service loop
 * (the common case, once it is up) and the direct-access fallback paths
 * below (bootstrap, `p9share`, or a dead uart task) -- both go through the
 * same uart_hw_*_blocking() primitives, so at most one waiter per direction
 * still holds regardless of who the caller is. A second, concurrent
 * blocking call in the same direction falls back to plain polling rather
 * than being silently queued -- the same "single-slot, busy refuses" shape
 * chan_call() itself uses for the same reason.
 *
 * interrupt-shared: a slot is taken by the task about to block, with
 * interrupts masked and its claim word held, and given back by uart_isr() or
 * by the waiter itself. The counters are written only in the handler or with
 * interrupts masked. */
static volatile int g_rx_waiter = -1;
static volatile int g_tx_waiter = -1;
static volatile uint32_t g_uart_irq_count;    /* see uart_irq_count()    */
static volatile uint32_t g_uart_irq_rx_wakes; /* see uart_irq_rx_wakes() */
static volatile uint32_t g_uart_irq_tx_arms;  /* see uart_irq_tx_arms()  */
static volatile uint32_t g_uart_irq_tx_wakes; /* see uart_irq_tx_wakes() */
static volatile uint32_t g_uart_irq_tx_seen;  /* see uart_irq_tx_seen()  */
/* Who owns each slot, across harts: irq_save() alone makes "is it free, then
 * take it" atomic on one hart only, and two printers on two harts could both
 * take it, the overwritten one never to be woken. Held for the whole wait. */
static volatile uint32_t g_rx_claim, g_tx_claim;
/* interrupt-shared: IER and the two waiter slots. IER is read-modify-written
 * by the handler and by both waiters, which can be on three harts: irq_save()
 * excludes the handler only on its own, and the RX and TX waiters not at all
 * -- one's enable could overwrite the other's, leaving it blocked with its
 * interrupt off. So every IER update, together with the slot it goes with, is
 * under g_ier_lock; the handler wakes its tasks after dropping it (a spinlock
 * is a leaf, and task_unblock() takes the scheduler's). */
static spinlock_t g_ier_lock;

static void ier_arm(volatile int *slot, int me, uint8_t bit) {
    uintptr_t f = spin_lock_irqsave(&g_ier_lock);
    *slot = me;
    uart_base[UART_IER] |= bit;
    spin_unlock_irqrestore(&g_ier_lock, f);
}

/* After the wait: a slot still naming `me` was not served by the handler. */
static void ier_disarm(volatile int *slot, int me, uint8_t bit) {
    uintptr_t f = spin_lock_irqsave(&g_ier_lock);
    if (*slot == me) {
        *slot = -1;
        uart_base[UART_IER] &= (uint8_t)~bit;
    }
    spin_unlock_irqrestore(&g_ier_lock, f);
}

static void uart_isr(void *ctx) {
    (void)ctx;
    g_uart_irq_count++;
    int rx = -1, tx = -1;
    uintptr_t f = spin_lock_irqsave(&g_ier_lock);
    uint8_t lsr = uart_base[UART_LSR];
    if (lsr & UART_LSR_THRE) g_uart_irq_tx_seen++;
    if ((lsr & UART_LSR_DR) && g_rx_waiter >= 0) {
        uart_base[UART_IER] &= (uint8_t)~UART_IER_ERBFI;
        g_uart_irq_rx_wakes++;
        rx = g_rx_waiter;
        g_rx_waiter = -1;
    }
    if ((lsr & UART_LSR_THRE) && g_tx_waiter >= 0) {
        uart_base[UART_IER] &= (uint8_t)~UART_IER_ETBEI;
        g_uart_irq_tx_wakes++;
        tx = g_tx_waiter;
        g_tx_waiter = -1;
    }
    spin_unlock_irqrestore(&g_ier_lock, f);
    if (lsr & UART_LSR_OE) note_rx_overrun();   /* prints: outside the lock */
    if (rx >= 0) task_unblock(rx);
    if (tx >= 0) task_unblock(tx);
}

// Raw hardware byte access, handed to drivers/uart_net.c's A3b demux at
// init time. Kept separate from uart_has_char()/uart_getc() below so the
// demux (when a user opts into it via `p9share`) can pull straight from the
// hardware register without going through the console-ring indirection it
// is itself responsible for providing.
static bool hw_uart_has_char(void) {
    if (!uart_base) return false;
    uint8_t lsr = uart_base[UART_LSR];
    if (lsr & UART_LSR_OE) note_rx_overrun();
    return (lsr & UART_LSR_DR) != 0;
}

static uint8_t hw_uart_getc(void) {
    return uart_base[UART_RBR];
}

/* The actual hardware wait/transfer,  * pulled out of uart_putc()/uart_getc() so both the uart task's own service
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
        /* sched_has_task(): a hart with no task cannot be the waiter --
         * task_block() would have nothing to block, and the slot would
         * hold -1, which reads as "free". It polls instead, which is
         * the branch already here for the other-waiter case. */
        if (sched_has_task() && arch_lock_try_acquire(&g_tx_claim)) {
            /* Prepared, and looped on the condition: on two harts the ISR
             * can run on the other one and wake this task before it has
             * blocked (phase31 §7 T11; sched.h's task_prepare_block()). */
            int me = sched_current_pid();
            do {
                task_prepare_block();
                g_uart_irq_tx_arms++;
                ier_arm(&g_tx_waiter, me, UART_IER_ETBEI);
                task_block();
            } while ((uart_base[UART_LSR] & UART_LSR_THRE) == 0);
            task_wait_done();
            ier_disarm(&g_tx_waiter, me, UART_IER_ETBEI);   /* woken by something else */
            arch_lock_release(&g_tx_claim);
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
        /* sched_has_task(): a hart with no task cannot be the waiter --
         * task_block() would have nothing to block, and the slot would
         * hold -1, which reads as "free". It polls instead, which is
         * the branch already here for the other-waiter case. */
        if (sched_has_task() && arch_lock_try_acquire(&g_rx_claim)) {
            int me = sched_current_pid();          /* as for TX above */
            do {
                task_prepare_block();
                ier_arm(&g_rx_waiter, me, UART_IER_ERBFI);
                task_block();
            } while (!hw_uart_has_char());
            task_wait_done();
            ier_disarm(&g_rx_waiter, me, UART_IER_ERBFI);
            arch_lock_release(&g_rx_claim);
        } else {
            irq_restore(flags);
            while (!hw_uart_has_char()) sched_yield();
            flags = irq_save();
        }
    }
    irq_restore(flags);
    return hw_uart_getc();
}

/* --- UART driver task ---
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

/* Must match (or exceed) uart_putc()'s UART_TX_BATCH_CAP, plus the opcode
 * byte -- this is the largest single 'W' request the endpoint can accept. */
#define UART_REQ_CAP (256 + 1)

static uint8_t       g_uart_req[UART_REQ_CAP];
static uint8_t       g_uart_resp[1];
/* single task: the boot (or the root shell re-running it); constant after. */
static driver_task_t g_uart_task;

/* Verify: counts UART_REQ_WRITE calls actually served -- one per
 * uart_flush() that reached the task (not per character, and not per
 * uart_putc()), so a test can check IPC volume tracks *lines/messages*
 * instead of guessing from timing. See uart_write_call_count(). */
static uint32_t g_uart_write_calls;   /* single task: the uart task (uart_serve()) */

uint32_t uart_write_call_count(void) { return g_uart_write_calls; }
uint32_t uart_irq_count(void) { return g_uart_irq_count; }
uint32_t uart_irq_rx_wakes(void) { return g_uart_irq_rx_wakes; }
uint32_t uart_irq_tx_arms(void) { return g_uart_irq_tx_arms; }
uint32_t uart_irq_tx_wakes(void) { return g_uart_irq_tx_wakes; }
uint32_t uart_irq_tx_seen(void) { return g_uart_irq_tx_seen; }

/* Found while bringing up drivers/uart_rp2350.c's own uart task on
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
static volatile bool g_uart_write_in_flight;   /* single task: the uart task; read by writers */

static bool uart_task_alive(void) { return driver_task_alive(&g_uart_task); }

/* This task, and only this task, may call uart_hw_putc_blocking()/
 * uart_hw_getc_blocking() while alive -- everything else reaches the
 * hardware through chan_call("uart", ...) via the facade functions below.
 * Must never call uart_putc()/uart_getc()/uart_has_char() itself: that
 * would chan_call() into its own endpoint, which chan_call()'s busy flag
 * correctly refuses rather than deadlocks, but there is no reason to rely
 * on that -- it should simply never happen. */
/* printk() here is fine. cprintf()/printk_debug() are not.
 *
 * This comment has been rewritten twice and the change is the point. It used
 * to forbid printk() outright, because printk() reached the console through
 * this very endpoint -- a caller blocked here while holding the output lock,
 * and this task taking that same lock to log, is a deadlock against itself.
 * Y4 made that cycle *named* rather
 * than silent; Y5c removed it. printk() is a ring append that reaches no
 * blocking primitive from any context, so this loop may log freely.
 *
 * What survives is the console stream: cprintf() and printk_debug() take
 * console_lock() and end at the wire, so calling either from here is the
 * original hazard in the one place it still exists. **Checked**, not merely
 * remembered -- drivers/driver_task.c brackets this callback and
 * console_lock() reports it (kernel/lock.h).
 *
 * uart_debug_putc()/uart_debug_puts() remain the escape hatch for getting
 * bytes out of a context that may not take the lock at all. */
static uint32_t uart_serve(void *ctx, const uint8_t *req, uint32_t req_len,
                          uint8_t *resp, uint32_t resp_cap) {
    (void)ctx; (void)resp_cap;
    switch (req[0]) {
        case UART_REQ_HASCHAR:
            resp[0] = hw_uart_has_char() ? 1 : 0;
            return 1;
        case UART_REQ_READ:
            resp[0] = uart_hw_getc_blocking();
            return 1;
        case UART_REQ_WRITE:
            /* This driver's own counter, not the framework's: it counts
             * WRITE calls specifically, which is what makes "IPC volume
             * tracks lines, not characters" a checkable claim.
             * driver_task_call_count() counts every request served. */
            g_uart_write_calls++;
            g_uart_write_in_flight = true;
            for (uint32_t i = 1; i < req_len; i++) {
                uart_hw_putc_blocking((char)req[i]);
            }
            g_uart_write_in_flight = false;
            return 0;
        default:
            return 0;
    }
}

/* Called from kernel/main.c, after sched_init() (task_create() needs a
 * table to create into) -- unlike uart_init() below, which runs long
 * before that and only brings the hardware itself up. Failure here is not
 * fatal: every caller's facade function falls back to direct hardware
 * access whenever uart_task_alive() is false, so a board that could not
 * start this task for some reason keeps working exactly as it did before
 * Just without the isolation the task provides. */
int uart_task_start(void) {
    const driver_task_spec_t spec = {
        .name        = "uart",
        .serve       = uart_serve,
        .ctx         = NULL,
        .req         = g_uart_req,  .req_cap  = sizeof(g_uart_req),
        .resp        = g_uart_resp, .resp_cap = sizeof(g_uart_resp),
        /* An empty request is answered, not served: it reaches no hardware
         * and is not a call. */
        .min_req_len = 1,
        /* shallow call depth, 1 page is generous. */
        .stack_pages = 1,
        /* TASK_PRIO_INTERRUPT: a caller that chan_call()s "uart" unblocks
         * this task, then blocks itself (kernel/chan.c's chan_call_task())
         * -- at TASK_PRIO_NORMAL like everything else, next_runnable() could
         * pick any other ready task first, sitting the caller behind an
         * arbitrary queue for what is meant to be a near-instant hardware
         * handoff. sched.h reserved this tier for exactly this role. */
        .priority    = TASK_PRIO_INTERRUPT,
    };
    return driver_task_start(&g_uart_task, &spec);
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

/* The bounded retry moved to driver_task_call(), which every driver task shares. Why
 * bounded and yielding rather than blocking or falling straight back to a
 * hardware access that would race the request already in flight is written
 * there. What was specific to this driver was only the reason busy is rare
 * here: its writers are already serialized by uart_putc()'s own batch lock
 * below, and it has exactly one reader in practice.
 *
 * uart_flush() below does NOT use it -- see its own comment. */

/* batches characters into a chunk and sends one chan_call() per chunk
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
 * SYS_PUTCHAR) call this directly, without going through console_lock(), so
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
/* One batch per hart.
 *
 * It was a single shared buffer, and on two cores that spliced messages
 * together character by character: console_unlock() releases the output lock
 * and then flushes, so the other core acquires the lock and starts appending
 * into the same buffer in between, and this core's flush picks up both.
 * Observed on RP2350 as core 0 and core 1 interleaving mid-word.
 *
 * The obvious fix -- flush while still holding the output lock -- trades one
 * bug for a worse one: the flush blocks in chan_call() waiting for the uart
 * task, so the lock would be held across that wait, and anything the uart
 * task itself needs the console for would deadlock against it. Splitting the
 * buffer removes the sharing instead of serialising it, and no lock is held
 * across a block. Costs MAX_HARTS x 256 bytes.
 *
 * The lock stays: it still guards each hart's own buffer against preemption
 * on that hart, which is what it was added for. */
/* guarded by: g_tx_batch_lock (below); uart_flush_critical() takes its own
 * hart's batch without it when the lock stays held (a fault with it held). */
static char     g_tx_batch[MAX_HARTS][UART_TX_BATCH_CAP];
static uint32_t g_tx_batch_len[MAX_HARTS];
/* Guards g_tx_batch/g_tx_batch_len.
 *
 * X2 pins every driver task to hart 0, and S5 left the UART
 * paths unconverted on the strength of that. Checking the claim rather than
 * inheriting it -- which is what X2 is for -- shows it covers less than S5
 * assumed. It is true of the *waiter* path below: uart_hw_putc_blocking()
 * is reached only from the driver task itself, except on the fallback taken
 * when that task is absent or the p9share demux owns the wire.
 *
 * It is NOT true of this batch. uart_flush() and uart_putc() run in the
 * context of whoever is printing -- console_flush() calls the first, every
 * console write the second -- so on two harts two printing tasks touch this
 * buffer at once, and irq_save() never covered that. Same shape as
 * usb_cdc_putc(), and found the same way: by asking who actually calls it
 * rather than which file it lives in. */
static spinlock_t g_tx_batch_lock;

/* What keeps one task's bytes in order across harts (47.8). The batches being
 * per hart, a task moved between harts mid-line -- or mid-SLIP-frame -- used
 * to leave the first part in one hart's batch and send the rest, flushed,
 * from the other's: on the LCD-7B, 9P replies whose closing END overtook
 * their last bytes. So uart_putc() holds its caller on the hart
 * (sched_hold_current_on_hart()) and the holds on a hart are given back once
 * that hart has nothing batched and nothing in flight: a copy taken by
 * uart_flush() counts here until it is sent, or the owner could be released
 * and overtake it from the other hart.
 * guarded by: g_tx_batch_lock. */
static uint32_t g_tx_inflight[MAX_HARTS];

static void uart_tx_send(const char *local, uint32_t len) {
    if (uart_demux_is_enabled() || !uart_task_alive()) {
        for (uint32_t i = 0; i < len; i++) uart_hw_putc_blocking(local[i]);
        return;
    }
    uint8_t req[1 + UART_TX_BATCH_CAP];
    req[0] = UART_REQ_WRITE;
    memcpy(&req[1], local, len);
    uint8_t resp[1];
    /* Not driver_task_call()'s plain bounded retry: see
     * g_uart_write_in_flight's own comment for why a WRITE specifically
     * must never give up and fall back to direct access while another
     * WRITE is actually in flight, while still falling back promptly --
     * not waiting out a human's keystroke -- when the endpoint is merely
     * busy with a pending READ instead. */
    for (;;) {
        int n = chan_call(driver_task_endpoint(&g_uart_task), req, 1 + len, resp, sizeof(resp));
        if (n >= 0) return;
        if (!g_uart_write_in_flight) break;
        sched_yield();
    }
    for (uint32_t i = 0; i < len; i++) uart_hw_putc_blocking(local[i]);
}

void uart_flush(void) {
    char local[UART_TX_BATCH_CAP];
    uintptr_t flags = spin_lock_irqsave(&g_tx_batch_lock);
    unsigned h = hart_id();
    uint32_t len = g_tx_batch_len[h];
    if (len > 0) {
        memcpy(local, g_tx_batch[h], len);
        g_tx_batch_len[h] = 0;
        g_tx_inflight[h]++;
    }
    spin_unlock_irqrestore(&g_tx_batch_lock, flags);
    if (len == 0) return;
    uart_tx_send(local, len);

    flags = spin_lock_irqsave(&g_tx_batch_lock);
    g_tx_inflight[h]--;
    if (g_tx_batch_len[h] == 0 && g_tx_inflight[h] == 0) sched_release_hart_holds(h);
    spin_unlock_irqrestore(&g_tx_batch_lock, flags);
}

void uart_putc(char c) {
    uintptr_t flags = spin_lock_irqsave(&g_tx_batch_lock);
    while (g_tx_batch_len[hart_id()] >= UART_TX_BATCH_CAP) {
        /* Released around uart_flush(), which takes this same lock --
         * spinlock_t is not re-entrant, and the re-check on re-acquire is
         * what makes dropping it safe (the existing comment above the batch
         * explains why the re-check was already required). */
        spin_unlock_irqrestore(&g_tx_batch_lock, flags);
        uart_flush();
        flags = spin_lock_irqsave(&g_tx_batch_lock);
    }
    g_tx_batch[hart_id()][g_tx_batch_len[hart_id()]++] = c;
    sched_hold_current_on_hart();
    spin_unlock_irqrestore(&g_tx_batch_lock, flags);
}

// When the A3b demux is enabled (opt-in, via `p9share`), the console can no
// longer read the hardware register directly -- some of what's waiting
// there may be 9P frame bytes. uart_demux_console_has_char() pumps the
// hardware and returns only the console-routed subset; disabled (the
// default), this now asks the uart task the same question previously asked the
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

#define UART_HASCHAR_WAIT_MS 2u

bool uart_has_char(void) {
    uart_flush();
    if (uart_demux_is_enabled()) return uart_demux_console_has_char();
    if (uart_task_alive()) {
        /* A short turn: the call ahead may be a READ waiting for a key. Then
         * the status register is read here; reading it changes nothing the
         * task depends on. */
        uint8_t req[1] = { UART_REQ_HASCHAR };
        uint8_t resp[1];
        if (driver_task_call_wait(&g_uart_task, req, 1, resp, 1, UART_HASCHAR_WAIT_MS) == 1)
            return resp[0] != 0;
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
        /* Blocking by contract, so it waits for its turn as long as that takes
         * (a READ ahead of it waits for a key): a second reader of the RX
         * register beside the task would take keys from it. A refusal (no
         * task to block, a spinlock held) still returns at once. */
        uint8_t req[1] = { UART_REQ_READ };
        uint8_t resp[1];
        if (driver_task_call_wait(&g_uart_task, req, 1, resp, 1, UINT32_MAX) == 1)
            return (char)resp[0];
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
/* See drivers/uart.h. Bounded spin on THRE, then drop. No yield, no block. */
void uart_critical_putc(char c) {
    if (!uart_base) return;
    for (unsigned i = 0; i < 200000u; i++) {
        if (uart_base[UART_LSR] & UART_LSR_THRE) {
            uart_base[UART_THR] = (uint8_t)c;
            return;
        }
    }
}

void uart_critical_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_critical_putc('\r');
        uart_critical_putc(*s++);
    }
}

/* See drivers/uart.h. Same batch as uart_flush(), written the bounded way. */
/* Never blocks, and that is the whole requirement.
 *
 * Every printk_critical() calls this first, and printk_critical() is what the
 * fatal trap handler reports through. Taking the batch lock unconditionally
 * meant a hart that died while holding it silenced the crash report of the
 * hart still running -- the machine stopped mid-dump with no indication that
 * anything further had been attempted. Each captured instance of the `exec`
 * wild jump ends exactly this way: two lines of register dump, then nothing,
 * with the lines that would have named the faulting task never printed. The
 * bug being reported is not this one, but this one is why the reports were
 * unreadable.
 *
 * On a failed acquire the drain still happens. The batch slot is indexed by
 * hart and no hart touches another's, so reading our own is correct without
 * the lock; what the lock buys is *ordering* between two harts draining at
 * once, and interleaved characters in a panic beat a panic nobody sees. */
void uart_flush_critical(void) {
    char local[UART_TX_BATCH_CAP];
    uintptr_t flags;
    bool locked = spin_trylock_irqsave(&g_tx_batch_lock, &flags, 1000000u);
    unsigned h = hart_id();
    uint32_t len = g_tx_batch_len[h];
    if (len > 0) {
        memcpy(local, g_tx_batch[h], len);
        g_tx_batch_len[h] = 0;
    }
    if (locked) {
        spin_unlock_irqrestore(&g_tx_batch_lock, flags);
    } else {
        irq_restore(flags);
    }
    for (uint32_t i = 0; i < len; i++) uart_critical_putc(local[i]);
}

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
