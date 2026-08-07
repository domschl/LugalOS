#ifndef LUGALOS_KERNEL_PRINTK_H
#define LUGALOS_KERNEL_PRINTK_H

#include <stdarg.h>
#include <stdint.h>

int printk(const char *fmt, ...);

// Physical-UART-only diagnostics: never mirrored to a USB CDC console. Use
// this (not printk()) for low-level driver tracing that could itself be
// caused by, or cause, USB traffic (e.g. inside drivers/usb_cdc.c) -- mixing
// that into a mirrored output can create a feedback loop. printk() remains
// the general-purpose, mirrored text output for kernel/shell/user-facing
// messages.
int printk_debug(const char *fmt, ...);

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
int ksnprintf(char *buf, uint32_t cap, const char *fmt, ...);

#endif /* LUGALOS_KERNEL_PRINTK_H */
