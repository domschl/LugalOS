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

/* Y5b, plan/phase31_concurrency_hierarchy.md: the destination carries a
 * context pointer.
 *
 * This is what lets printk() format into a buffer on its *own stack* before
 * storing the result as one log record. ksnprintf()'s comment below records
 * why the context-free form could not: it reached its buffer through "a
 * single shared pointer -- not reentrant". printk() is re-entered for real --
 * an interrupt handler that prints lands inside an outer printk(), and a
 * preempted task may resume on the other hart -- so neither a static pointer
 * nor a per-hart one is safe. A parameter threaded down the call chain is.
 *
 * Callers that write straight out pass NULL and ignore it. */
typedef void (*putc_fn)(void *ctx, char);
typedef void (*puts_fn)(void *ctx, const char *);

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
 * console_lock()/console_unlock() (kernel/console.h) replace the mask with what the B6
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

/* printk_lock()/printk_unlock() are gone (Y5d,
 * plan/phase31_concurrency_hierarchy.md).
 *
 * They were a third blocking primitive, hand-rolled: a single owner, a single
 * waiter slot, a re-entrancy depth, a polling fallback for a hart with no
 * task, a spinlock guarding all of it, and -- since Y4 -- an edge in the
 * wait-for graph maintained by hand. `ylock_t` already had every one of those
 * and had them checked, so the console's lock is one (kernel/console.h's
 * console_lock()), and the graph is back to two contributors: channels and
 * ylocks.
 *
 * What made the deletion possible was Y5b and Y5c rather than anything here.
 * The lock existed to hold a whole message together across a char-at-a-time
 * emission that could block; a message is one record appended under a leaf
 * spinlock now, and the part that blocks belongs to klogd. printk() takes no
 * lock at all -- what is left to serialise is the console *stream*, whose
 * writers are cprintf(), printk_debug() and the drain, and that is console.c's
 * business rather than printk.c's.
 *
 * The hard-won comment that used to sit on printk_unlock()'s flush is kept
 * where the flush went, in console_flush(): the batch is per hart precisely
 * so that nothing is held across the block inside uart_flush().
 */

/* Adapter for the destinations that were already plain function pointers
 * and have no state to carry -- the UART, the console, the debug port. The
 * struct lives on the caller's stack, so this costs nothing static. */
typedef void (*raw_putc_fn)(char);
typedef void (*raw_puts_fn)(const char *);

typedef struct { raw_putc_fn pc; raw_puts_fn ps; } plain_dest_t;

static void plain_putc(void *ctx, char c)          { ((plain_dest_t *)ctx)->pc(c); }
static void plain_puts(void *ctx, const char *str) { ((plain_dest_t *)ctx)->ps(str); }

static void print_num(putc_fn pc, void *ctx, unsigned long num, int base) {
    char buf[64];
    const char digits[] = "0123456789abcdef";
    int i = 0;

    if (num == 0) {
        pc(ctx, '0');
        return;
    }

    while (num > 0) {
        buf[i++] = digits[num % base];
        num /= base;
    }

    while (i > 0) {
        pc(ctx, buf[--i]);
    }
}

static void print_timestamp(putc_fn pc, puts_fn ps, void *ctx) {
    uint64_t ms = time_get_ms();
    unsigned int sec = (unsigned int)(ms / 1000);
    unsigned int msec = (unsigned int)(ms % 1000);

    ps(ctx, "[");
    if (sec < 10) ps(ctx, "    ");
    else if (sec < 100) ps(ctx, "   ");
    else if (sec < 1000) ps(ctx, "  ");
    else if (sec < 10000) ps(ctx, " ");

    print_num(pc, ctx, sec, 10);
    pc(ctx, '.');
    pc(ctx, '0' + ((msec / 100) % 10));
    pc(ctx, '0' + ((msec / 10) % 10));
    pc(ctx, '0' + (msec % 10));
    ps(ctx, "] ");
}

static int vprintk_to(putc_fn pc, puts_fn ps, void *ctx,
                      const char *fmt, va_list args, bool with_ts) {
    if (!fmt) return -1;

    if (with_ts && fmt[0] == '[' && fmt[1] != '\0') {
        print_timestamp(pc, ps, ctx);
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
            pc(ctx, *p);
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
                if (!left_pad) { for (int w = 1; w < width; w++) pc(ctx, ' '); }
                pc(ctx, c);
                if (left_pad)  { for (int w = 1; w < width; w++) pc(ctx, ' '); }
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                int len = 0;
                while (s[len] != '\0' && (max_len < 0 || len < max_len)) len++;
                if (!left_pad) { for (int w = len; w < width; w++) pc(ctx, ' '); }
                for (int i = 0; i < len; i++) pc(ctx, s[i]);
                if (left_pad)  { for (int w = len; w < width; w++) pc(ctx, ' '); }
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
                    for (int w = 0; w < width - digits; w++) pc(ctx, pad);
                }
                if (val < 0) {
                    pc(ctx, '-');
                    val = -val;
                }
                print_num(pc, ctx, (unsigned long)val, 10);
                if (width > digits && left_pad) {
                    for (int w = 0; w < width - digits; w++) pc(ctx, ' ');
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
                    for (int w = 0; w < width - digits; w++) pc(ctx, pad);
                }
                print_num(pc, ctx, val, 10);
                if (width > digits && left_pad) {
                    for (int w = 0; w < width - digits; w++) pc(ctx, ' ');
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
                    for (int w = 0; w < width - digits; w++) pc(ctx, pad);
                }
                print_num(pc, ctx, val, 16);
                if (width > digits && left_pad) {
                    for (int w = 0; w < width - digits; w++) pc(ctx, ' ');
                }
                break;
            }
            case '%':
                pc(ctx, '%');
                break;
            default:
                pc(ctx, '%');
                pc(ctx, *p);
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
/* The destination printk() formats into: a buffer on its own stack (Y5b). */
typedef struct {
    char    *buf;
    uint32_t cap;
    uint32_t len;
    bool     overflowed;
} rec_dest_t;

static void rec_putc(void *ctx, char c) {
    rec_dest_t *d = (rec_dest_t *)ctx;
    if (d->len < d->cap) d->buf[d->len++] = c;
    else                 d->overflowed = true;
}

static void rec_puts(void *ctx, const char *s) {
    if (!s) return;
    while (*s) rec_putc(ctx, *s++);
}

static int vprintk_ctx_buffered(rec_dest_t *d, const char *fmt, va_list args) {
    return vprintk_to(rec_putc, rec_puts, d, fmt, args, false);
}

int printk(const char *fmt, ...) {
    char buf[KLOG_REC_MAX];
    rec_dest_t d = { buf, sizeof(buf), 0, false };

    /* The timestamp travels as four binary bytes in the record header rather
     * than twelve rendered characters in the payload, so it is taken here and
     * the formatter is told to leave it out. Same condition vprintk_to() has
     * always applied: a message prefixed with a bracketed tag is stamped, a
     * continuation line or a banner is not. */
    uint32_t ms = KLOG_NO_TS;
    if (fmt && fmt[0] == '[' && fmt[1] != '\0') {
        ms = (uint32_t)time_get_ms();
    }

    va_list args;
    va_start(args, fmt);
    /* No lock at all (Y5c, plan/phase31_concurrency_hierarchy.md).
     *
     * There is nothing left for it to protect. Formatting happens in `buf` on
     * this stack, the append is atomic under the ring's own leaf spinlock,
     * and the fan-out -- the part that could block, and the reason a lock had
     * to be held across the whole message -- belongs to klogd now.
     *
     * This is the line that makes printk() callable from anywhere: from a
     * driver task mid-serve, from an interrupt handler, with a spinlock held.
     * None of them can block here, because there is no longer anything here
     * to block on. */
    int ret = vprintk_ctx_buffered(&d, fmt, args);

    /* Truncation is marked in the output and counted, never silent: the tail
     * of the longest, most detailed diagnostic in the tree is exactly what
     * must not disappear without saying so. The marker replaces the last four
     * bytes rather than extending the record, since the record is full by
     * definition at this point. */
    if (d.overflowed) {
        if (d.len >= 4) {
            d.buf[d.len - 4] = '.';
            d.buf[d.len - 3] = '.';
            d.buf[d.len - 2] = '.';
            d.buf[d.len - 1] = '\n';
        }
        klog_truncated();
    }

    klog_emit(ms, buf, d.len);
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
    console_lock();
    int ret = vprintk_to(plain_putc, plain_puts, &(plain_dest_t){ uart_debug_putc, uart_debug_puts }, fmt, args, true);
    console_unlock();
    console_flush();
    va_end(args);
    return ret;
}

/* Output that has reached the wire before the next instruction runs.
 *
 * **This comment was rewritten by Y5d, and what it used to say is the
 * interesting part.** printk() had two blocking points -- the ownership lock,
 * and the uart_flush() underneath it -- so printk_critical() existed because
 * printk() could not be called from scheduler teardown, from interrupt
 * context, or under g_sched_lock. All three of those are now fine: printk()
 * appends a record to the log ring and returns, from anywhere.
 *
 * One reason survives, and it is the whole reason:
 *
 *   **printk() is delivered, eventually, by klogd. This is delivered now.**
 *
 * A record in the ring reaches the console when the consumer next runs. If
 * the next thing that happens is a halt, a fault dump, or a hang, the
 * consumer never runs and the record is only readable afterwards from
 * /proc/kmsg -- on a board that may not be answering. Anything whose value
 * is that it arrived *before* the machine stopped belongs here: fault dumps,
 * the lock checker's own reports, panic paths. "The ship is already sinking
 * in that case anyway" (user, 2026-09-11), which is exactly why this path
 * stays synchronous while everything else stopped being.
 *
 * So this takes no lock and does not flush. It writes straight at the
 * hardware through uart_critical_putc(), which spins on the transmit FIFO
 * for a bounded count and then drops the byte.
 *
 * Two consequences, both deliberate:
 *
 *   - **Output can interleave** with a concurrent console write from another
 *     hart or from the task this interrupted. Taking the output lock is what
 *     would prevent that, and taking a lock is the thing this path must not
 *     do. Garbled diagnostics beat absent ones.
 *   - **Output can be dropped** if the console is wedged or absent. A
 *     console must not be able to stop the kernel.
 *
 * It does still record into the klog ring (klog_record(), ring only, no sink
 * fan-out) and it does still honour whether the terminal sink is attached --
 * so `/proc/kmsg` keeps the message and `klog detach console` still silences
 * it. What it skips is the fan-out itself, because the console sink's putc is
 * uart_putc(), which batches and blocks once the batch fills. */
/* printk_critical() keeps writing the UART character by character as it
 * formats, deliberately (Y5b). Buffering it first would mean a fault partway
 * through formatting emits nothing at all, and this is the one path whose
 * whole guarantee is that what you read reached the wire before the halt.
 *
 * The ring copy is accumulated alongside and stored as one record at the end;
 * if the machine dies mid-message the UART already has it and the ring does
 * not, which is the right way round. */
static char     g_crit_buf[KLOG_REC_MAX];
static uint32_t g_crit_len;

static void crit_stash(char c) { if (g_crit_len < sizeof(g_crit_buf)) g_crit_buf[g_crit_len++] = c; }

static void critical_putc_ring(void *ctx, char c) { (void)ctx; crit_stash(c); }
static void critical_putc_both(void *ctx, char c) { (void)ctx; crit_stash(c); uart_critical_putc(c); }
static void critical_puts_ring(void *ctx, const char *s) { while (*s) critical_putc_ring(ctx, *s++); }
static void critical_puts_both(void *ctx, const char *s) { while (*s) critical_putc_both(ctx, *s++); }

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
    g_crit_len = 0;
    int ret = tty ? vprintk_to(critical_putc_both, critical_puts_both, NULL, fmt, args, true)
                  : vprintk_to(critical_putc_ring, critical_puts_ring, NULL, fmt, args, true);
    va_end(args);

    /* The record goes in whole, after the wire already has it. The timestamp
     * is KLOG_NO_TS because vprintk_to() rendered one into the text above --
     * this path prints as it formats, so it cannot defer the stamp the way
     * printk() does. */
    if (g_crit_len > 0) klog_record_text(KLOG_NO_TS, g_crit_buf, g_crit_len, tty);
    return ret;
}

/* User-facing output (B4). Same engine as printk(), different stream: this
 * lands on whatever device the console is bound to and is unaffected by
 * kernel-log sink changes. Splitting the two is what makes
 * `klog detach console` silence diagnostics without silencing the shell. */
int cprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    /* Drain *inside* the lock, not before it (Y5d).
     *
     * Both orders flush the log before this call's text. Only this one makes
     * the pair atomic: with the drain outside, another writer -- klogd, or a
     * cprintf() on the other hart -- could interpose between the drain and the
     * write, so a log line from seconds earlier landed in the middle of a
     * command's output. The suite found it as a different single test failing
     * on each run, which is what a race looks like when the thing it corrupts
     * is whatever happened to be printing.
     *
     * Re-entrant: klog_drain() takes this same ylock, and a ylock is
     * re-entrant for its owner. */
    console_lock();
    klog_drain();
    int ret = vprintk_to(plain_putc, plain_puts, &(plain_dest_t){ console_putc, console_puts }, fmt, args, true);
    console_unlock();
    console_flush();
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

static void snprintf_putc(void *ctx, char c) {
    (void)ctx;
    if (g_snprintf_ctx.idx < g_snprintf_ctx.cap - 1) {
        g_snprintf_ctx.buf[g_snprintf_ctx.idx++] = c;
    }
}

static void snprintf_puts(void *ctx, const char *s) {
    if (!s) return;
    while (*s) snprintf_putc(ctx, *s++);
}

int ksnprintf(char *buf, uint32_t cap, const char *fmt, ...) {
    if (!buf || cap == 0) return 0;

    g_snprintf_ctx.buf = buf;
    g_snprintf_ctx.idx = 0;
    g_snprintf_ctx.cap = cap;

    va_list args;
    va_start(args, fmt);
    vprintk_to(snprintf_putc, snprintf_puts, NULL, fmt, args, true);
    va_end(args);

    buf[g_snprintf_ctx.idx] = '\0';
    return (int)g_snprintf_ctx.idx;
}
