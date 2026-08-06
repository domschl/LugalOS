#ifndef LUGALOS_KERNEL_PRINTK_H
#define LUGALOS_KERNEL_PRINTK_H

#include <stdarg.h>

int printk(const char *fmt, ...);

// Physical-UART-only diagnostics: never mirrored to a USB CDC console. Use
// this (not printk()) for low-level driver tracing that could itself be
// caused by, or cause, USB traffic (e.g. inside drivers/usb_cdc.c) -- mixing
// that into a mirrored output can create a feedback loop. printk() remains
// the general-purpose, mirrored text output for kernel/shell/user-facing
// messages.
int printk_debug(const char *fmt, ...);

#endif /* LUGALOS_KERNEL_PRINTK_H */
