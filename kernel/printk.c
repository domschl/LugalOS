#include "kernel/printk.h"
#include "kernel/klog.h"
#include "kernel/console.h"
#include "drivers/uart.h"
#include "kernel/time.h"
#include <stdint.h>
#include "kernel/irq.h"

typedef void (*putc_fn)(char);
typedef void (*puts_fn)(const char *);

/* One printk/cprintf call emits as one uninterrupted run (B6).
 *
 * Preemption made a message a shared resource. Two tasks formatting at once
 * interleave character by character, and the result is not merely untidy: it
 * splices words together, so a marker a test greps for ("UPROG_TEXT_OK")
 * arrives cut in half and the test fails for a reason that has nothing to do
 * with what it was testing. The RP2350 hardware suite hit exactly that.
 *
 * Masking interrupts rather than taking a lock, for the same reasons
 * kernel/irq.h gives: the region is short, there is one hart, and printk() is
 * reachable from the trap handler -- where a lock that could block would be a
 * deadlock rather than a wait. irq_save()/irq_restore() nest, so a printk
 * from inside an already-masked region is still correct.
 *
 * Formatting happens inside the region too. Splitting "format into a buffer,
 * then emit" would shorten the masked window, but every sink here is either a
 * ring-buffer append (RP2350's USB CDC) or a QEMU MMIO store, so the window
 * is already short and the extra buffer would cost stack in the trap path. */

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
            if (*p == '\n') {
                pc('\r');
            }
            pc(*p);
            continue;
        }

        p++; // Skip '%'
        bool zero_pad = false;
        if (*p == '0') {
            zero_pad = true;
            p++;
        }

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
            case 'c':
                pc((char)va_arg(args, int));
                break;
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                if (max_len >= 0) {
                    for (int i = 0; i < max_len && s[i] != '\0'; i++) {
                        pc(s[i]);
                    }
                } else {
                    ps(s);
                }
                break;
            }
            case 'd':
            case 'i': {
                long val = va_arg(args, long);
                long temp = (val < 0) ? -val : val;
                int digits = (val <= 0) ? 1 : 0;
                while (temp != 0) { digits++; temp /= 10; }
                if (width > digits) {
                    char pad = zero_pad ? '0' : ' ';
                    for (int w = 0; w < width - digits; w++) pc(pad);
                }
                if (val < 0) {
                    pc('-');
                    val = -val;
                }
                print_num(pc, (unsigned long)val, 10);
                break;
            }
            case 'u': {
                unsigned long val = va_arg(args, unsigned long);
                unsigned long temp = val;
                int digits = (val == 0) ? 1 : 0;
                while (temp != 0) { digits++; temp /= 10; }
                if (width > digits) {
                    char pad = zero_pad ? '0' : ' ';
                    for (int w = 0; w < width - digits; w++) pc(pad);
                }
                print_num(pc, val, 10);
                break;
            }
            case 'x':
            case 'X':
            case 'p': {
                unsigned long val = va_arg(args, unsigned long);
                unsigned long temp = val;
                int digits = (val == 0) ? 1 : 0;
                while (temp != 0) { digits++; temp /= 16; }
                if (width > digits) {
                    char pad = zero_pad ? '0' : ' ';
                    for (int w = 0; w < width - digits; w++) pc(pad);
                }
                print_num(pc, val, 16);
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
    uintptr_t flags = irq_save();
    int ret = vprintk_to(klog_putc, klog_puts_shim, fmt, args);
    irq_restore(flags);
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
    uintptr_t flags = irq_save();
    int ret = vprintk_to(uart_debug_putc, uart_debug_puts, fmt, args);
    irq_restore(flags);
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
    uintptr_t flags = irq_save();
    int ret = vprintk_to(console_putc, console_puts, fmt, args);
    irq_restore(flags);
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
