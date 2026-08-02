#include "kernel/printk.h"
#include "drivers/uart.h"
#include <stdint.h>

static void print_num(unsigned long num, int base) {
    char buf[64];
    const char digits[] = "0123456789abcdef";
    int i = 0;

    if (num == 0) {
        uart_putc('0');
        return;
    }

    while (num > 0) {
        buf[i++] = digits[num % base];
        num /= base;
    }

    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

int printk(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            if (*p == '\n') {
                uart_putc('\r');
            }
            uart_putc(*p);
            continue;
        }

        p++; // Skip '%'
        while (*p >= '0' && *p <= '9') p++; // Skip width specifier digits (e.g., %12u)
        if (*p == 'l') p++; // Handle %ld / %lx / %lu

        switch (*p) {
            case 'c':
                uart_putc((char)va_arg(args, int));
                break;
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                uart_puts(s);
                break;
            }
            case 'd':
            case 'i': {
                long val = va_arg(args, long);
                if (val < 0) {
                    uart_putc('-');
                    val = -val;
                }
                print_num((unsigned long)val, 10);
                break;
            }
            case 'u':
                print_num(va_arg(args, unsigned long), 10);
                break;
            case 'x':
            case 'p':
                print_num(va_arg(args, unsigned long), 16);
                break;
            case '%':
                uart_putc('%');
                break;
            default:
                uart_putc('%');
                uart_putc(*p);
                break;
        }
    }

    va_end(args);
    return 0;
}
