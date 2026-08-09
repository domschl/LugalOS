#include "kernel/console.h"

/* See kernel/include/kernel/console.h. The formatting engine lives in
 * kernel/printk.c and is shared; only the destination differs. */

static console_putc_fn g_console_putc;

void console_bind(console_putc_fn putc) {
    g_console_putc = putc;
}

void console_putc(char c) {
    if (g_console_putc) g_console_putc(c);
}

void console_puts(const char *s) {
    if (!s) return;
    while (*s) console_putc(*s++);
}
