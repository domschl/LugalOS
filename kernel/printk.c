#include "kernel/printk.h"
#include "kernel/klog.h"
#include "kernel/console.h"
#include "drivers/uart.h"
#include "kernel/time.h"
#include "kernel/sched.h"
#include <stdint.h>
#include "kernel/irq.h"
#include <string.h>
#include "kernel/lock.h"

typedef void (*putc_fn)(char);
typedef void (*puts_fn)(const char *);

/* One printk/cprintf/printk_debug call -- and, since M4, one console_putc()/
 * console_puts() call -- emits as one uninterrupted run (B6, revised M2.5,
 * extended M4 -- plan/phase12_microkernel_migration.md).
 *
 * Preemption made a message a shared resource. Two tasks formatting at once
 * interleave character by character, and the result is not merely untidy: it
 * splices words together, so a marker a test greps for ("UPROG_TEXT_OK")
 * arrives cut in half and the test fails for a reason that has nothing to do
 * with what it was testing. The RP2350 hardware suite hit exactly that.
 *
 * M4 turned uart_putc() from a direct hardware write into a chan_call() to
 * the uart task -- a real task_block()/task_unblock() round trip on every
 * single character, not merely a slim window an interrupt could land in.
 * kernel/console.c's console_putc()/console_puts() (the line editor's
 * redraws, SYS_PUTNUM/SYS_PUTCHAR's raw numeric output) called this
 * unprotected, on the pre-M4 assumption that a character write was too fast
 * to interleave with anything. Once every character write is a guaranteed
 * scheduling point, that assumption is false on every call, not just an
 * unlucky one -- which is why M4 widened this lock's callers rather than
 * giving console.c one of its own: two different locks guarding the same
 * wire would not stop each other's holders from interleaving.
 *
 * B6 originally masked interrupts here rather than taking a lock, and said
 * why: "the region is short... and printk() is reachable from the trap
 * handler -- where a lock that could block would be a deadlock rather than a
 * wait." That was correct as long as nothing inside the region could ever
 * block. M2 (plan/phase12_microkernel_migration.md) broke exactly that
 * invariant: uart_putc() started calling task_block() when TX backpressures,
 * and irq_save()/irq_restore() is a single global CPU bit, not a per-task
 * saved context -- masking across a context switch leaves interrupts off for
 * whichever *other* task now runs, not just the one that asked, with no
 * record of who still owes an irq_restore(). That is what actually produced
 * the interleaving this comment used to attribute to preemption alone:
 * preemption could never fire *during* a masked printk() before M2, because
 * nothing inside it yielded.
 *
 * printk_lock()/printk_unlock() below replace the mask with what the B6
 * comment said a lock would need to not be: one that can be held across a
 * real block. It works from the trap handler for the same reason ordinary
 * timer preemption already does -- an ISR runs on the interrupted task's own
 * stack, as that task, so task_block() there suspends the interrupted flow
 * exactly the way a preemption tick already can, and resumes it later at
 * the same point. The one real hazard is self-deadlock: an unhandled
 * interrupt's printk() (kernel/devirq.c's fallback) firing while the
 * interrupted task already holds this lock, from an outer printk() call it
 * is itself in the middle of. Handled by tracking the owner and letting the
 * same task re-enter for free rather than block on itself.
 *
 * Formatting happens inside the locked region too. Splitting "format into a
 * buffer, then emit" would shorten it, but every sink here is either a
 * ring-buffer append (RP2350's USB CDC) or a QEMU MMIO store, so the window
 * is already short and the extra buffer would cost stack in the trap path. */

/* Single owner, single waiter slot -- the same "one slot, busy refuses or
 * falls back to polling" shape as kernel/chan.c's endpoints and M2's UART
 * TX/RX waiters, not a general wait queue. Console output in this tree
 * realistically has at most a couple of concurrent producers (the active
 * shell/task plus a background server), so a queue would be machinery this
 * kernel does not need yet. */
static volatile int g_printk_owner   = -1;
static volatile int g_printk_depth  = 0;
static volatile int g_printk_waiter = -1;

/* S5, plan/phase22_smp_locking_foundation.md: guards the owner/depth/waiter
 * bookkeeping above, and nothing else.
 *
 * This lock is the *fourth* independent reinvention of the same idea the
 * audit found -- after fs/p9_link.c, kernel/chan.c and kernel/palloc.c, and
 * the most elaborate of them, because it grew a directed wakeup that the
 * others did not need. It is kept rather than replaced by a ylock_t: the
 * waiter hand-off below is a real property of the console path (M2's
 * blocking uart_putc()) and swapping it for ylock_t's yield-and-retry would
 * change the scheduling behaviour of every printk() in the system to fix a
 * problem that a four-byte lock fixes without touching it.
 *
 * What it fixes is the same thing everywhere in this phase: the check-then-
 * set of g_printk_owner was atomic only against a second access *from the
 * same hart*, because irq_save() is all that stood behind it. Two harts
 * both find g_printk_owner == -1 and both become the owner, and the console
 * interleaving M2 spent a debugging session on comes back -- except now it
 * cannot be reproduced by reasoning about one call stack.
 *
 * Held for a handful of instructions and never across the task_block()
 * below, which is the spinlock_t contract (kernel/lock.h). */
static spinlock_t g_printk_gate;

void printk_lock(void) {
    /* The *context*, not the pid. A hart still in bring-up owns no task, and
     * sched_current_pid() answers -1 there -- which is this lock's own
     * "unowned" value, so using it would make an owned lock look free and let
     * every hart straight in. sched_context_id() is never -1 and never
     * collides with a pid, so the ownership and re-entrancy tests below are
     * exactly as correct for a task-less hart as for a task.
     *
     * Until phase 23's identity fix that pid was reported as 0, which was
     * worse than either: a bring-up hart matched `g_printk_owner == me`
     * against a lock the boot task was holding on the *other* hart, took the
     * re-entrant path, and both harts wrote the console at once. */
    int me = sched_context_id();
    bool blockable = sched_has_task();
    uintptr_t flags = spin_lock_irqsave(&g_printk_gate);
    if (g_printk_owner == me) {
        /* Reentrant: the ISR-during-our-own-printk() case above. Proceeds
         * without blocking -- blocking here would be waiting for ourselves
         * to release a lock we cannot release until we return. */
        g_printk_depth++;
        spin_unlock_irqrestore(&g_printk_gate, flags);
        return;
    }
    while (g_printk_owner != -1) {
        if (blockable && g_printk_waiter < 0) {
            int owner = g_printk_owner;
            g_printk_waiter = me;
            /* Released before blocking, never held across it. */
            spin_unlock_irqrestore(&g_printk_gate, flags);

            /* Y4, plan/phase31_concurrency_hierarchy.md: printk ownership is
             * the *third* blocking resource, after channels and ylocks, and
             * until now the only one outside the wait-for graph. §0.2 noted
             * it -- "a lock without being either type" -- and the uart
             * drivers each carry a comment saying the cycle through it must
             * be avoided by hand, because "chan.c's wait-for cycle guard
             * covers chan_call() itself, but printk_lock() is a different
             * blocking resource it cannot see".
             *
             * It can see it now. The edge is this task waiting for the owner,
             * exactly like the other two, which buys two things: a chan_call()
             * that would close the cycle from the other side is *refused*
             * outright, and a cycle that closes here is named rather than
             * silent.
             *
             * Named, not prevented: printk_unlock() clears ownership
             * unconditionally, so "proceed without owning it" is not
             * representable without adding state to the most safety-critical
             * path in the kernel for a case that should never happen. The
             * same bargain Y2 struck for spinlocks -- report before the wait
             * that hangs, so the board says which two tasks rather than
             * simply stopping. */
            bool edge = waitfor_enter(me, owner);
            if (!edge) waitfor_report_cycle("printk_lock()", me, owner);

            task_block();
            if (edge) waitfor_leave(me);
            flags = spin_lock_irqsave(&g_printk_gate);
        } else {
            /* Someone else is already the registered waiter -- or this hart
             * owns no task and so cannot be one. Fall back to polling rather
             * than overwrite their slot and lose their wakeup, which is the
             * same defensive shape as M2's UART waiters. Polling terminates
             * for a task-less hart because the owner is by definition a task
             * running somewhere else, and sched_yield() from here is a plain
             * spin. */
            spin_unlock_irqrestore(&g_printk_gate, flags);
            sched_yield();
            flags = spin_lock_irqsave(&g_printk_gate);
        }
    }
    g_printk_owner = me;
    g_printk_depth = 1;
    spin_unlock_irqrestore(&g_printk_gate, flags);
}

void printk_unlock(void) {
    uintptr_t flags = spin_lock_irqsave(&g_printk_gate);
    if (g_printk_depth > 1) {
        g_printk_depth--;
        spin_unlock_irqrestore(&g_printk_gate, flags);
        return;
    }
    g_printk_depth = 0;
    g_printk_owner = -1;
    int waiter = g_printk_waiter;
    g_printk_waiter = -1;
    spin_unlock_irqrestore(&g_printk_gate, flags);
    if (waiter >= 0) task_unblock(waiter);

    /* M4: send whatever this message batched into uart_putc()'s buffer
     * (drivers/uart.h) now that the message is complete -- the *outermost*
     * unlock only, matching "one message, one flush" rather than flushing
     * once per reentrant printk() nested inside another. Not the only
     * place this is called (uart_getc()/uart_has_char() also flush, for
     * output that never goes through printk_lock() at all -- line editor
     * redraws, raw console_putc() sequences), but the natural one for the
     * printk/cprintf/printk_debug family specifically.
     *
     * After the release, deliberately, and phase 23 X7 is why that is safe
     * on two cores rather than merely traditional.
     *
     * On one shared batch buffer it was not safe: releasing here let the
     * other core acquire the lock and start appending into the same buffer,
     * and this core's flush then sent both messages spliced together --
     * observed on RP2350 as core 0 and core 1 interleaving mid-word. That
     * looked exactly like a failed lock and was not; `smpstart locktest`
     * shows amoswap excluding the two cores correctly.
     *
     * The tempting fix was to flush while still holding ownership. It trades
     * this bug for a worse one: uart_flush() blocks in chan_call() waiting
     * for the uart task, so printk ownership would be held across that wait,
     * and anything the uart task needs printk for deadlocks against it. The
     * batch is per-hart instead (drivers/uart_*.c), which removes the
     * sharing rather than serialising it, and nothing is held across a
     * block. */
    uart_flush();
}

static void print_num(putc_fn pc, unsigned long num, int base) {
    char buf[64];
    const char digits[] = "0123456789abcdef";
    int i = 0;

    if (num == 0) {
        pc('0');
        return;
    }

    while (num > 0) {
        buf[i++] = digits[num % base];
        num /= base;
    }

    while (i > 0) {
        pc(buf[--i]);
    }
}

static void print_timestamp(putc_fn pc, puts_fn ps) {
    uint64_t ms = time_get_ms();
    unsigned int sec = (unsigned int)(ms / 1000);
    unsigned int msec = (unsigned int)(ms % 1000);

    ps("[");
    if (sec < 10) ps("    ");
    else if (sec < 100) ps("   ");
    else if (sec < 1000) ps("  ");
    else if (sec < 10000) ps(" ");

    print_num(pc, sec, 10);
    pc('.');
    pc('0' + ((msec / 100) % 10));
    pc('0' + ((msec / 10) % 10));
    pc('0' + (msec % 10));
    ps("] ");
}

static int vprintk_to(putc_fn pc, puts_fn ps, const char *fmt, va_list args) {
    if (!fmt) return -1;

    if (fmt[0] == '[' && fmt[1] != '\0') {
        print_timestamp(pc, ps);
    }

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            /* Raw '\n', deliberately. This used to emit '\r' first, which
             * looked like it handled the terminal convention but only ever
             * applied to newlines written literally here -- bytes passed
             * through %s went out untranslated, so cprintf("%s\n", file)
             * printed a staircase. The convention now belongs to the console
             * stream (kernel/console.h's console_emit), which sees every byte
             * regardless of how it got here. */
            pc(*p);
            continue;
        }

        p++; // Skip '%'
        /* Flags. '-' (left-justify) used to be missing entirely, which is
         * worse than unsupported: an unrecognised flag left the '-' sitting
         * where the conversion character was expected, so `%-4s` fell through
         * to the default branch and printed itself literally. Found by a
         * column-aligned diagnostic printing "%-4s %-6s" instead of its own
         * data (phase17 C1). */
        bool left_pad = false;
        bool zero_pad = false;
        for (;;) {
            if (*p == '-')      { left_pad = true; p++; }
            else if (*p == '0') { zero_pad = true; p++; }
            else break;
        }
        if (left_pad) zero_pad = false;   /* '0' is meaningless left-justified */

        int width = -1;
        if (*p >= '1' && *p <= '9') {
            width = 0;
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }
        }
        int max_len = -1;
        if (*p == '.') {
            p++;
            max_len = 0;
            while (*p >= '0' && *p <= '9') {
                max_len = max_len * 10 + (*p - '0');
                p++;
            }
        }
        if (*p == 'l') p++; // Handle %ld / %lx / %lu

        switch (*p) {
            case 'c': {
                char c = (char)va_arg(args, int);
                if (!left_pad) { for (int w = 1; w < width; w++) pc(' '); }
                pc(c);
                if (left_pad)  { for (int w = 1; w < width; w++) pc(' '); }
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                int len = 0;
                while (s[len] != '\0' && (max_len < 0 || len < max_len)) len++;
                if (!left_pad) { for (int w = len; w < width; w++) pc(' '); }
                for (int i = 0; i < len; i++) pc(s[i]);
                if (left_pad)  { for (int w = len; w < width; w++) pc(' '); }
                break;
            }
            case 'd':
            case 'i': {
                long val = va_arg(args, long);
                long temp = (val < 0) ? -val : val;
                int digits = (val <= 0) ? 1 : 0;
                while (temp != 0) { digits++; temp /= 10; }
                if (width > digits && !left_pad) {
                    char pad = zero_pad ? '0' : ' ';
                    for (int w = 0; w < width - digits; w++) pc(pad);
                }
                if (val < 0) {
                    pc('-');
                    val = -val;
                }
                print_num(pc, (unsigned long)val, 10);
                if (width > digits && left_pad) {
                    for (int w = 0; w < width - digits; w++) pc(' ');
                }
                break;
            }
            case 'u': {
                unsigned long val = va_arg(args, unsigned long);
                unsigned long temp = val;
                int digits = (val == 0) ? 1 : 0;
                while (temp != 0) { digits++; temp /= 10; }
                if (width > digits && !left_pad) {
                    char pad = zero_pad ? '0' : ' ';
                    for (int w = 0; w < width - digits; w++) pc(pad);
                }
                print_num(pc, val, 10);
                if (width > digits && left_pad) {
                    for (int w = 0; w < width - digits; w++) pc(' ');
                }
                break;
            }
            case 'x':
            case 'X':
            case 'p': {
                unsigned long val = va_arg(args, unsigned long);
                unsigned long temp = val;
                int digits = (val == 0) ? 1 : 0;
                while (temp != 0) { digits++; temp /= 16; }
                if (width > digits && !left_pad) {
                    char pad = zero_pad ? '0' : ' ';
                    for (int w = 0; w < width - digits; w++) pc(pad);
                }
                print_num(pc, val, 16);
                if (width > digits && left_pad) {
                    for (int w = 0; w < width - digits; w++) pc(' ');
                }
                break;
            }
            case '%':
                pc('%');
                break;
            default:
                pc('%');
                pc(*p);
                break;
        }
    }

    return 0;
}

// General-purpose kernel/shell text output. Since B0 this goes to the kernel
// log ring and fans out to whatever sinks are currently attached, rather than
// calling uart_putc() directly -- so output survives a UART being handed to
// 9P or to a login shell, and is readable afterwards via /proc/kmsg (see
// kernel/klog.h). Boot attaches the "console" sink, whose putc is uart_putc,
// so the default destination is byte-identical to the pre-B0 behavior: the
// physical UART plus the USB CDC console it already mirrored to.
static void klog_puts_shim(const char *s) {
    if (!s) return;
    while (*s) klog_putc(*s++);
}

int printk(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printk_lock();
    int ret = vprintk_to(klog_putc, klog_puts_shim, fmt, args);
    printk_unlock();
    va_end(args);
    return ret;
}

// Low-level driver/kernel diagnostics only (see printk.h): physical UART
// only, never mirrored to USB. Used by drivers/usb_cdc.c's own tracing so
// that logging USB activity can't itself become USB traffic that logs more
// activity.
//
// Deliberately NOT routed through klog (B0), unlike printk() above: this
// function's entire purpose is to bypass the mirroring machinery, and the
// log ring is served by /proc/kmsg, which a remote 9P client can read. Low
// level USB/I2C/SPI tracing does not belong in a file other nodes fetch, and
// keeping it on the direct path preserves its "physical UART, always, no
// exceptions" guarantee without needing an argument about sink policy.
int printk_debug(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printk_lock();
    int ret = vprintk_to(uart_debug_putc, uart_debug_puts, fmt, args);
    printk_unlock();
    va_end(args);
    return ret;
}

/* Output from a context that must not block, yield, or switch.
 *
 * printk() has two blocking points and both are load-bearing for everything
 * else. printk_lock() calls task_block() when another task owns the lock and
 * sched_yield() when it cannot block; printk_unlock() then calls
 * uart_flush(), which reaches the console through chan_call() and blocks
 * again. That is exactly right for ordinary kernel logging and exactly wrong
 * in three places:
 *
 *   - **Scheduler teardown.** task_exit()'s first act used to be a printk(),
 *     so a task announcing its own death switched away in the middle of
 *     dying -- BLOCKED, still owning its stack, with none of its exit
 *     bookkeeping done -- and a second task could enter task_exit() behind
 *     it. That is a deterministic crash, found on the ESP32-P4 the day
 *     preemption started working there (phase 27 E4).
 *   - **Interrupt context.** kernel/devirq.c's unhandled-IRQ report and the
 *     trap handler's own diagnostics run from the interrupt path, where
 *     task_block() has nothing to be woken by. Moving task_exit()'s message
 *     into sched_reap() looked like the obvious fix and hung the board,
 *     because sched_reap() is called from sched_yield(), which the timer
 *     interrupt calls.
 *   - **Fatal handlers, and anything holding the scheduler lock.** A dump
 *     that blocks is a dump that never prints, and a printk() under
 *     g_sched_lock deadlocks against the task it is waiting for.
 *
 * So this takes no lock and does not flush. It writes straight at the
 * hardware through uart_critical_putc(), which spins on the transmit FIFO
 * for a bounded count and then drops the byte.
 *
 * Two consequences, both deliberate:
 *
 *   - **Output can interleave** with a concurrent printk() from another
 *     hart or from the task this interrupted. Taking the ownership lock is
 *     what would prevent that, and taking it is the thing that deadlocks.
 *     Garbled diagnostics beat absent ones.
 *   - **Output can be dropped** if the console is wedged or absent. A
 *     console must not be able to stop the kernel.
 *
 * It does still record into the klog ring (klog_record(), ring only, no sink
 * fan-out) and it does still honour whether the terminal sink is attached --
 * so `/proc/kmsg` keeps the message and `klog detach console` still silences
 * it. What it skips is the fan-out itself, because the console sink's putc is
 * uart_putc(), which batches and blocks once the batch fills. */
static void critical_putc_ring(char c) { klog_record(c); }
static void critical_putc_both(char c) { klog_record(c); uart_critical_putc(c); }
static void critical_puts_ring(const char *s) { while (*s) critical_putc_ring(*s++); }
static void critical_puts_both(const char *s) { while (*s) critical_putc_both(*s++); }

/* Is the terminal sink currently attached?
 *
 * printk_critical() cannot fan out through klog's sinks -- see klog_record()
 * -- but it must still obey the policy those sinks express, or `klog detach
 * console` stops meaning anything. It is the same question klog_putc() asks
 * implicitly by iterating `attached` sinks, asked once per message instead of
 * once per character. An unknown answer errs toward printing: a diagnostic
 * from a fatal path is worth more than a tidy terminal. */
static bool critical_terminal_attached(void) {
    for (uint32_t i = 0; i < 8; i++) {
        const char *name = NULL;
        bool attached = false;
        if (!klog_sink_info(i, &name, &attached)) break;
        if (name && name[0] == 'c' && strcmp(name, "console") == 0) return attached;
    }
    return true;
}

int printk_critical(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    bool tty = critical_terminal_attached();
    /* Anything already batched goes out first, so this message cannot
     * overtake output that was produced before it. See uart_flush_critical(). */
    if (tty) uart_flush_critical();
    int ret = tty ? vprintk_to(critical_putc_both, critical_puts_both, fmt, args)
                  : vprintk_to(critical_putc_ring, critical_puts_ring, fmt, args);
    va_end(args);
    return ret;
}

/* User-facing output (B4). Same engine as printk(), different stream: this
 * lands on whatever device the console is bound to and is unaffected by
 * kernel-log sink changes. Splitting the two is what makes
 * `klog detach console` silence diagnostics without silencing the shell. */
int cprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printk_lock();
    int ret = vprintk_to(console_putc, console_puts, fmt, args);
    printk_unlock();
    va_end(args);
    return ret;
}

/* ksnprintf() reuses vprintk_to() with buffer-backed putc/puts callbacks
 * instead of UART ones. putc_fn/puts_fn take no context parameter (they're
 * plain function pointers, matching uart_putc/uart_puts), so there's no way
 * for snprintf_putc() below to know *which* buffer to write to except via a
 * single shared pointer -- not reentrant, but nothing in this freestanding,
 * single-threaded kernel calls printk() from inside a format callback, so
 * that's never exercised in practice. */
static struct {
    char *buf;
    uint32_t idx;
    uint32_t cap; /* buf[cap - 1] is reserved for the terminating NUL */
} g_snprintf_ctx;

static void snprintf_putc(char c) {
    if (g_snprintf_ctx.idx < g_snprintf_ctx.cap - 1) {
        g_snprintf_ctx.buf[g_snprintf_ctx.idx++] = c;
    }
}

static void snprintf_puts(const char *s) {
    if (!s) return;
    while (*s) snprintf_putc(*s++);
}

int ksnprintf(char *buf, uint32_t cap, const char *fmt, ...) {
    if (!buf || cap == 0) return 0;

    g_snprintf_ctx.buf = buf;
    g_snprintf_ctx.idx = 0;
    g_snprintf_ctx.cap = cap;

    va_list args;
    va_start(args, fmt);
    vprintk_to(snprintf_putc, snprintf_puts, fmt, args);
    va_end(args);

    buf[g_snprintf_ctx.idx] = '\0';
    return (int)g_snprintf_ctx.idx;
}
