/* See drivers/radio/uprintf.h. 45.3b, plan/phase45_esp32c6.md. */

#include "uprintf.h"
#include <stdbool.h>
#include <stddef.h>

/* U-mode text placement: empty on the host and in the kernel; the radio build
 * sets it to the section its domain grants execute on. */
#ifndef RADIO_TEXT
#define RADIO_TEXT
#endif

typedef struct { char *buf; uint32_t cap, len; } out_t;

static RADIO_TEXT void put(out_t *o, char c) {
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

/* Digits of `v` in `base`, least significant first, into `tmp`.
 *
 * Schoolbook division of the 64-bit value by `base` (<= 16), one 16-bit limb at
 * a time, using only 32-bit arithmetic. Not for show: on rv32 a 64-bit `/`, `%`
 * *or shift* is a call into libgcc (__udivdi3, __lshrdi3, ...), which a U-mode
 * text page cannot reach -- the first version of this function faulted on its
 * first shift. tools/check_radio_text.py now checks that nothing in the radio's
 * objects calls anything outside them. */
static RADIO_TEXT int digits(unsigned long long v, unsigned base, bool upper, char *tmp) {
    uint32_t limb[4] = { (uint32_t)(v >> 48) & 0xffffu, (uint32_t)(v >> 32) & 0xffffu,
                         (uint32_t)(v >> 16) & 0xffffu, (uint32_t)v & 0xffffu };
    int n = 0;
    for (;;) {
        bool zero = true;
        uint32_t rem = 0;
        for (int i = 0; i < 4; i++) {
            uint32_t cur = rem * 65536u + limb[i];
            limb[i] = cur / base;
            rem = cur % base;
            if (limb[i]) zero = false;
        }
        tmp[n++] = (char)(rem < 10 ? '0' + rem : (upper ? 'A' : 'a') + (rem - 10));
        if (zero) break;
    }
    return n;
}

RADIO_TEXT int ku_vsnprintf(char *buf, uint32_t cap, const char *fmt, va_list ap) {
    out_t o = { buf, cap, 0 };
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { put(&o, *p); continue; }
        p++;
        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;; p++) {
            if (*p == '-') left = true;
            else if (*p == '0') zero = true;
            else if (*p == '+') plus = true;
            else if (*p == ' ') space = true;
            else if (*p == '#') alt = true;
            else break;
        }
        int width = 0, prec = -1;
        if (*p == '*') { width = va_arg(ap, int); if (width < 0) { left = true; width = -width; } p++; }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (*p == '.') {
            p++; prec = 0;
            if (*p == '*') { prec = va_arg(ap, int); p++; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        int lng = 0;                                   /* 0 int, 1 long, 2 long long */
        bool shrt = false, chr = false, sz = false;
        for (;; p++) {
            if (*p == 'l') lng++;
            else if (*p == 'h') { if (shrt) chr = true; shrt = true; }
            else if (*p == 'z' || *p == 't' || *p == 'j') sz = true;
            else break;
        }
        char conv = *p;
        if (!conv) break;

        char tmp[72];
        const char *s = tmp;
        int slen = 0, pad_prefix = 0;
        char prefix[3]; int plen = 0;
        bool numeric = false;

        switch (conv) {
        case '%': put(&o, '%'); continue;
        case 'c': tmp[0] = (char)va_arg(ap, int); slen = 1; break;
        case 's': {
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while ((prec < 0 || slen < prec) && s[slen]) slen++;
            break;
        }
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long) : lng == 1 || sz ? va_arg(ap, long) : va_arg(ap, int);
            if (!lng && !sz) { if (chr) v = (signed char)v; else if (shrt) v = (short)v; }
            unsigned long long m = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
            if (v < 0) prefix[plen++] = '-'; else if (plus) prefix[plen++] = '+'; else if (space) prefix[plen++] = ' ';
            slen = digits(m, 10, false, tmp); numeric = true;
            if (m == 0 && prec == 0) slen = 0;          /* C: a zero with precision 0 prints nothing */
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'p': {
            unsigned long long v;
            if (conv == 'p') { v = (unsigned long long)(uintptr_t)va_arg(ap, void *); alt = true; conv = 'x'; }
            else v = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 || sz ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            if (!lng && !sz && !shrt) v = (unsigned)v;
            else if (!lng && !sz) v = chr ? (unsigned char)v : (unsigned short)v;
            unsigned base = conv == 'o' ? 8 : conv == 'u' ? 10 : 16;
            if (alt && v != 0) {
                if (base == 16) { prefix[plen++] = '0'; prefix[plen++] = conv; }
            }
            slen = digits(v, base, conv == 'X', tmp); numeric = true;
            if (v == 0 && prec == 0 && !(alt && base == 8)) slen = 0;   /* but `%#.0o` of 0 is "0" */
            /* `%#o`: a leading zero is guaranteed by raising the precision, not by
             * a prefix -- so `%#.4o` of 255 is 0377, not 00377. */
            if (alt && base == 8 && v != 0 && prec < slen + 1) prec = slen + 1;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            (void)va_arg(ap, double);
            tmp[0] = '?'; slen = 1; break;
        default:                                       /* an unknown conversion prints as itself */
            put(&o, '%'); put(&o, conv); continue;
        }

        int zeros = 0;
        if (numeric) {
            if (prec >= 0) { if (prec > slen) zeros = prec - slen; zero = false; }
            else if (zero && !left) { int total = plen + slen; if (width > total) zeros = width - total; }
        }
        pad_prefix = width - (plen + zeros + slen);
        if (pad_prefix < 0) pad_prefix = 0;
        if (!left) while (pad_prefix-- > 0) put(&o, ' ');
        for (int i = 0; i < plen; i++) put(&o, prefix[i]);
        while (zeros-- > 0) put(&o, '0');
        if (numeric) for (int i = slen - 1; i >= 0; i--) put(&o, tmp[i]);   /* digits are reversed */
        else for (int i = 0; i < slen; i++) put(&o, s[i]);
        if (left) while (pad_prefix-- > 0) put(&o, ' ');
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

RADIO_TEXT int ku_snprintf(char *buf, uint32_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = ku_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}
