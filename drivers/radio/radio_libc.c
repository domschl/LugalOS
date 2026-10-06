/* The handful of libc and libm functions the Wi-Fi blob calls that the chip's ROM
 * does not provide (45.6, plan/phase45_esp32c6.md). They are *the radio
 * domain's own*, linked into its text with the rest of the shim: the blob runs
 * in U-mode, where a call into the kernel's libc is an instruction access fault,
 * and the list is exactly what `tools/c6_blob_spike/link.sh` reports unresolved.
 *
 * Nothing here may use floating point (this core has none and the kernel boots
 * with mstatus.FS = 0), call a compiler helper, or allocate other than from the
 * radio heap: tools/check_radio_text.py reads this object like the others.
 * The loop-pattern pass is switched off per function, because GCC at -O2 turns a
 * byte-copy loop into a call to memcpy -- inside memcpy. */

#include "osi_impl.h"
#include "uprintf.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define LIBC_TEXT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

LIBC_TEXT void *radio_memcpy(void *d, const void *s, size_t n) {
    uint8_t *dp = d; const uint8_t *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

LIBC_TEXT void *radio_memmove(void *d, const void *s, size_t n) {
    uint8_t *dp = d; const uint8_t *sp = s;
    if (dp == sp || n == 0) return d;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}

LIBC_TEXT int radio_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}

LIBC_TEXT char *radio_strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) { }
    return r;
}

LIBC_TEXT char *radio_strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

LIBC_TEXT int radio_strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (uint8_t)*a < (uint8_t)*b ? -1 : 1;
        if (!*a) return 0;
    }
    return 0;
}

LIBC_TEXT void radio_free(void *p) { radio_osi_free(p); }
LIBC_TEXT void *radio_malloc(size_t n) { return radio_osi_malloc(n); }
LIBC_TEXT void *radio_calloc(size_t n, size_t m) { return radio_osi_calloc(n, m); }
LIBC_TEXT void *radio_realloc(void *p, size_t n) { return radio_osi_realloc(p, n); }

LIBC_TEXT size_t radio_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
LIBC_TEXT int radio_strcmp(const char *a, const char *b) {
    for (; *a && *a == *b; a++, b++) { }
    return (uint8_t)*a < (uint8_t)*b ? -1 : (uint8_t)*a > (uint8_t)*b;
}
LIBC_TEXT char *radio_strchr(const char *s, int c) {
    for (; ; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; }
}
LIBC_TEXT char *radio_strrchr(const char *s, int c) {
    const char *r = 0;
    for (; ; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; }
}
LIBC_TEXT char *radio_strstr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return 0;
}
LIBC_TEXT long radio_strtol(const char *s, char **end, int base) {
    long v = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0) base = (s[0] == '0') ? 8 : 10;
    for (;; s++) {
        int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'z') ? *s - 'a' + 10 : (*s >= 'A' && *s <= 'Z') ? *s - 'A' + 10 : 99;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
LIBC_TEXT int radio_atoi(const char *s) { return (int)radio_strtol(s, 0, 10); }
LIBC_TEXT int radio_vsnprintf(char *b, size_t n, const char *f, va_list ap) { return ku_vsnprintf(b, (uint32_t)n, f, ap); }
LIBC_TEXT int radio_snprintf(char *b, size_t n, const char *f, ...) {
    va_list ap; va_start(ap, f); int r = ku_vsnprintf(b, (uint32_t)n, f, ap); va_end(ap); return r;
}
/* A radio that aborts ends its thread (the kernel logs the exit); there is nothing to unwind to. */
LIBC_TEXT void radio_abort(void) {
    radio_osi_log_write(1, "radio", "abort()");
    register long a0 __asm__("a0") = 20;           /* SYS_UEXIT (kernel/ipc.h) */
    __asm__ volatile("ecall" : "+r"(a0) :: "memory");
    for (;;) { }
}
LIBC_TEXT void __assert_func(const char *file, int line, const char *func, const char *expr) {
    radio_osi_log_write(1, "radio", "assert %s:%d %s: %s", file, line, func, expr);
    radio_abort();
}
/* libgcc's byte swap, which a U-mode text cannot call. */
LIBC_TEXT unsigned __bswapsi2(unsigned x) { return (x >> 24) | ((x >> 8) & 0xff00u) | ((x << 8) & 0xff0000u) | (x << 24); }

/* Formatting and the blob's own log sinks go to the shim's formatter and, from
 * there, one ecall per line. The four *_printf are the blob libraries' log hooks. */
LIBC_TEXT int radio_sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = ku_vsnprintf(buf, 0x7fffffffu, fmt, ap);
    va_end(ap);
    return n;
}

static int blob_log(const char *fmt, va_list ap) {
    radio_osi_log_writev(3, "blob", fmt, ap);
    return 0;
}
LIBC_TEXT int radio_pp_printf(const char *fmt, ...)       { va_list ap; va_start(ap, fmt); int r = blob_log(fmt, ap); va_end(ap); return r; }
LIBC_TEXT int radio_phy_printf(const char *fmt, ...)      { va_list ap; va_start(ap, fmt); int r = blob_log(fmt, ap); va_end(ap); return r; }
LIBC_TEXT int radio_net80211_printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = blob_log(fmt, ap); va_end(ap); return r; }
LIBC_TEXT int radio_coexist_printf(const char *fmt, ...)  { va_list ap; va_start(ap, fmt); int r = blob_log(fmt, ap); va_end(ap); return r; }

LIBC_TEXT int radio_puts(const char *s) { radio_osi_log_write(3, "blob", "%s", s); return 0; }
LIBC_TEXT int radio_putchar(int c) { radio_osi_log_write(3, "blob", "%c", c); return c; }

/* The crystal frequency in MHz (soc_xtal_freq_t's value). */
LIBC_TEXT int radio_rtc_clk_xtal_freq_get(void) { return 40; }

/* floor() for the one place the blob uses it, with no floating-point
 * instruction and no libgcc call: the IEEE-754 double taken apart in integers
 * (fdlibm's algorithm). A double is passed as two words in the soft-float ABI. */
LIBC_TEXT double radio_floor(double x) {
    union { double d; uint32_t w[2]; } u;
    u.d = x;
    int32_t i0 = (int32_t)u.w[1];
    uint32_t i1 = u.w[0];
    int32_t j0 = ((i0 >> 20) & 0x7ff) - 0x3ff;
    if (j0 < 20) {
        if (j0 < 0) {
            if (i0 >= 0) { i0 = 0; i1 = 0; }
            else if (((uint32_t)(i0 & 0x7fffffff) | i1) != 0) { i0 = (int32_t)0xbff00000; i1 = 0; }
        } else {
            uint32_t i = 0x000fffffu >> j0;
            if ((((uint32_t)i0 & i) | i1) == 0) return x;           /* already integral */
            if (i0 < 0) i0 += 0x00100000 >> j0;
            i0 &= ~(int32_t)i;
            i1 = 0;
        }
    } else if (j0 > 51) {
        return x;                                                    /* huge, inf or nan */
    } else {
        uint32_t i = 0xffffffffu >> (j0 - 20);
        if ((i1 & i) == 0) return x;
        if (i0 < 0) {
            if (j0 == 20) i0 += 1;
            else {
                uint32_t j = i1 + (1u << (52 - j0));
                if (j < i1) i0 += 1;
                i1 = j;
            }
        }
        i1 &= ~i;
    }
    u.w[1] = (uint32_t)i0;
    u.w[0] = i1;
    return u.d;
}

/* From wpa_supplicant's utils/common.c, the one function of it the blob wants at
 * link time (45.7 brings in the rest of the supplicant and this goes). */
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
LIBC_TEXT int radio_hexstr2bin(const char *hex, uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        int a = hexval(hex[2 * i]), b = a < 0 ? -1 : hexval(hex[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        buf[i] = (uint8_t)(a * 16 + b);
    }
    return 0;
}
