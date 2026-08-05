#include "kernel/printk.h"
#include "drivers/uart.h"
#include "kernel/time.h"
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

static void print_timestamp(void) {
    uint64_t ms = time_get_ms();
    unsigned int sec = (unsigned int)(ms / 1000);
    unsigned int msec = (unsigned int)(ms % 1000);

    uart_puts("[");
    if (sec < 10) uart_puts("    ");
    else if (sec < 100) uart_puts("   ");
    else if (sec < 1000) uart_puts("  ");
    else if (sec < 10000) uart_puts(" ");

    print_num(sec, 10);
    uart_putc('.');
    uart_putc('0' + ((msec / 100) % 10));
    uart_putc('0' + ((msec / 10) % 10));
    uart_putc('0' + (msec % 10));
    uart_puts("] ");
}

int printk(const char *fmt, ...) {
    if (!fmt) return -1;

    if (fmt[0] == '[' && fmt[1] != '\0') {
        print_timestamp();
    }

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
                uart_putc((char)va_arg(args, int));
                break;
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                if (max_len >= 0) {
                    for (int i = 0; i < max_len && s[i] != '\0'; i++) {
                        uart_putc(s[i]);
                    }
                } else {
                    uart_puts(s);
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
                    for (int w = 0; w < width - digits; w++) uart_putc(pad);
                }
                if (val < 0) {
                    uart_putc('-');
                    val = -val;
                }
                print_num((unsigned long)val, 10);
                break;
            }
            case 'u': {
                unsigned long val = va_arg(args, unsigned long);
                unsigned long temp = val;
                int digits = (val == 0) ? 1 : 0;
                while (temp != 0) { digits++; temp /= 10; }
                if (width > digits) {
                    char pad = zero_pad ? '0' : ' ';
                    for (int w = 0; w < width - digits; w++) uart_putc(pad);
                }
                print_num(val, 10);
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
                    for (int w = 0; w < width - digits; w++) uart_putc(pad);
                }
                print_num(val, 16);
                break;
            }
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
