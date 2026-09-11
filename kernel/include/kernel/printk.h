#ifndef LUGALOS_KERNEL_PRINTK_H
#define LUGALOS_KERNEL_PRINTK_H

#include <stdarg.h>
#include <stdint.h>

/* Compiler-checked format strings (Y5f, plan/phase31_concurrency_hierarchy.md
 * §5.9).
 *
 * This family had no `format(printf, ...)` attribute at all, so `-Wall
 * -Wextra` checked nothing across the whole tree -- and kernel/printk.c's
 * engine parses the `l` length modifier and *discards* it, which means `%ld`
 * on an int64 reads 32 bits and misaligns every argument after it. That cost
 * a garbled line; under Y5f's argument capture it would corrupt a record.
 *
 * Worth having on its own merits either way: it turns the tree's standing
 * "never %ld an int64" rule into a compile error. */
/* Enabled on 64-bit targets only, and that is not a cop-out -- it is where
 * the check is *accurate*.
 *
 * Measured: with the attribute on, rv64 builds with zero warnings while the
 * 32-bit targets produce 107-170, every one of them the same thing -- `%u`
 * passed a `uint32_t`, which this toolchain types as `unsigned long` on rv32.
 * Those are correct at run time (both are 32 bits) and wrong only by the
 * letter of the standard, so fixing them means ~170 mechanical `(unsigned)`
 * casts that buy nothing.
 *
 * The bugs worth catching are width mismatches -- `%d` handed an int64 reads
 * half of it and misaligns every argument after -- and those warn on rv64,
 * where `int64_t` is `long`. Since every target compiles the *same sources*,
 * a format bug anywhere in the tree is caught by the rv64 build. The check
 * runs where it can tell the difference, and the rv32 typedef noise stays
 * out of the zero-warning policy.
 *
 * If the 32-bit casts are ever done, drop the #if and this comment with it. */
#if __SIZEOF_LONG__ == 8
#  define LUGALOS_PRINTF(fmt_idx, first_arg) \
      __attribute__((format(printf, (fmt_idx), (first_arg))))
#else
#  define LUGALOS_PRINTF(fmt_idx, first_arg)
#endif

int printk(const char *fmt, ...) LUGALOS_PRINTF(1, 2);

// The lock printk()/cprintf()/printk_debug() take to make one call's output
// one uninterrupted run (see kernel/printk.c's top comment) -- exposed so
// kernel/console.c's console_putc()/console_puts() can take the same lock
// around raw console writes (the line editor's redraws, SYS_PUTNUM/
// SYS_PUTCHAR). Reentrant by task, so nesting under an outer printk() (or
// another console_putc()) is free rather than a self-deadlock.

// Physical-UART-only diagnostics: never mirrored to a USB CDC console. Use
// this (not printk()) for low-level driver tracing that could itself be
// caused by, or cause, USB traffic (e.g. inside drivers/usb_cdc.c) -- mixing
// that into a mirrored output can create a feedback loop. printk() remains
// the general-purpose, mirrored text output for kernel/shell/user-facing
// messages.
int printk_debug(const char *fmt, ...) LUGALOS_PRINTF(1, 2);

/* Synchronous: on the wire before the next instruction runs. Unlocked, and
 * drops rather than waits.
 *
 * Not "for contexts where printk() would block" any more -- printk() does not
 * block anywhere (Y5c, plan/phase31_concurrency_hierarchy.md). The remaining
 * distinction is delivery: a printk() record reaches the console when klogd
 * next runs, and if the next thing that happens is a halt, a fault dump or a
 * hang, klogd never runs. Use this where the value is that the line arrived
 * *before* the machine stopped; use printk() everywhere else, which is almost
 * everywhere. See kernel/printk.c for the full argument and what it costs
 * (interleaving, and dropped bytes on a wedged console). */
int printk_critical(const char *fmt, ...) LUGALOS_PRINTF(1, 2);

// Formats into a caller-owned buffer instead of a UART, using the same
// format-string engine as printk() (%s/%d/%u/%x/%c/%%, width, zero-pad,
// precision). Always NUL-terminates within `cap` and returns the number of
// bytes written (excluding the NUL), clamped to `cap - 1`. Used by
// fs/vfs_server.c to generate real, readable /proc file content instead of
// printk()'ing it directly (see A1 in plan/phase5_distributed_design.md).
//
// Note: like printk(), a literal '\n' in the format string itself becomes
// "\r\n" in the output (this reuses printk's own engine unmodified) -- so
// buffers built this way carry CRLF line endings, which is what a terminal
// re-printing them via "%s" expects, but is not plain-text-file convention.
// Harmless today (nothing consumes this content except printk() again);
// worth revisiting if/when a 9P server starts serving these buffers to a
// remote client that expects LF-only text.
int ksnprintf(char *buf, uint32_t cap, const char *fmt, ...) LUGALOS_PRINTF(3, 4);

/* Renders a log record captured by printk() (Y5f,
 * plan/phase31_concurrency_hierarchy.md §5.9): the format string it was given
 * and the arguments it stored, replayed through the same engine that would
 * have formatted them at the time.
 *
 * The ring keeps `fmt` as a pointer. That is safe because a format string is
 * a string literal in .rodata -- immortal, and unique enough to serve as the
 * record's identity without an enum anyone has to maintain. Arguments are
 * copied, `%s` included, because those are frequently stack buffers that will
 * be gone by the time the log is read. */
int printk_render(const char *fmt, const uint8_t *blob, uint32_t blob_len,
                  void (*out)(void *, char), void *ctx);

#endif /* LUGALOS_KERNEL_PRINTK_H */
