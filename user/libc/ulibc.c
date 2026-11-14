#include "stdlib.h"
#include "stdio.h"
#include "ctype.h"
#include "errno.h"
#include "string.h"
#include "usys.h"

#ifndef ULIBC_HEAP_BYTES
#define ULIBC_HEAP_BYTES (64u * 1024u)
#endif

int errno = 0;

static FILE g_stdin_file  = { 0, 0, 0 };
static FILE g_stdout_file = { 1, 0, 0 };
static FILE g_stderr_file = { 2, 0, 0 };

FILE *const stdin  = &g_stdin_file;
FILE *const stdout = &g_stdout_file;
FILE *const stderr = &g_stderr_file;

/* --- Heap allocator (first-fit with coalescing) --- */

typedef struct hdr {
    size_t size;       /* total block size including header */
    int    free;
    struct hdr *next;
} hdr_t;

#define HDR_ALIGN 16u
#define ALIGN_UP(x) (((x) + (HDR_ALIGN - 1u)) & ~(size_t)(HDR_ALIGN - 1u))
#define HDR_SIZE ALIGN_UP(sizeof(hdr_t))

static uint8_t g_heap[ULIBC_HEAP_BYTES] __attribute__((aligned(16)));
static hdr_t  *g_heap_head;

static void heap_init_once(void) {
    if (g_heap_head) return;
    g_heap_head = (hdr_t *)g_heap;
    g_heap_head->size = sizeof(g_heap);
    g_heap_head->free = 1;
    g_heap_head->next = NULL;
}

void *malloc(size_t size) {
    if (size == 0) size = 1;
    heap_init_once();
    size_t need = HDR_SIZE + ALIGN_UP(size);
    for (hdr_t *b = g_heap_head; b; b = b->next) {
        if (!b->free || b->size < need) continue;
        if (b->size >= need + HDR_SIZE + HDR_ALIGN) {
            hdr_t *split = (hdr_t *)((uint8_t *)b + need);
            split->size = b->size - need;
            split->free = 1;
            split->next = b->next;
            b->size = need;
            b->next = split;
        }
        b->free = 0;
        return (uint8_t *)b + HDR_SIZE;
    }
    errno = ENOMEM;
    return NULL;
}

void free(void *ptr) {
    if (!ptr) return;
    hdr_t *b = (hdr_t *)((uint8_t *)ptr - HDR_SIZE);
    b->free = 1;
    for (hdr_t *cur = g_heap_head; cur && cur->next; ) {
        if (cur->free && cur->next->free &&
            (uint8_t *)cur + cur->size == (uint8_t *)cur->next) {
            cur->size += cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > ((size_t)-1) / nmemb) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    hdr_t *b = (hdr_t *)((uint8_t *)ptr - HDR_SIZE);
    size_t old_payload = b->size - HDR_SIZE;
    if (old_payload >= size) return ptr;
    void *np = malloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, old_payload);
    free(ptr);
    return np;
}

/* --- ctype --- */

int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalpha(int c)  { return isupper(c) || islower(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isprint(int c)  { return c >= 0x20 && c <= 0x7e; }
int iscntrl(int c)  { return (c >= 0 && c < 0x20) || c == 0x7f; }
int ispunct(int c)  { return isprint(c) && !isalnum(c) && !isspace(c); }
int toupper(int c)  { return islower(c) ? (c - ('a' - 'A')) : c; }
int tolower(int c)  { return isupper(c) ? (c + ('a' - 'A')) : c; }

/* --- stdlib --- */

int abs(int j)   { return j < 0 ? -j : j; }
long labs(long j) { return j < 0 ? -j : j; }

static unsigned long long parse_uint_core(const char *s, char **endptr, int base,
                                          int *neg_out) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0') { base = 8; p++; }
        else { base = 10; }
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    unsigned long long acc = 0;
    const char *start = p;
    while (*p) {
        int d = -1;
        if (isdigit((unsigned char)*p)) d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        if (d < 0 || d >= base) break;
        acc = acc * (unsigned long long)base + (unsigned long long)d;
        p++;
    }
    if (endptr) *endptr = (char *)(p == start ? s : p);
    if (neg_out) *neg_out = neg;
    return acc;
}

long long strtoll(const char *nptr, char **endptr, int base) {
    int neg = 0;
    unsigned long long v = parse_uint_core(nptr, endptr, base, &neg);
    return neg ? -(long long)v : (long long)v;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    int neg = 0;
    unsigned long long v = parse_uint_core(nptr, endptr, base, &neg);
    return neg ? (0ULL - v) : v;
}

long strtol(const char *nptr, char **endptr, int base) {
    return (long)strtoll(nptr, endptr, base);
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtoull(nptr, endptr, base);
}

int atoi(const char *nptr) {
    return (int)strtol(nptr, NULL, 10);
}

static void swap_bytes(uint8_t *a, uint8_t *b, size_t size) {
    for (size_t i = 0; i < size; i++) {
        uint8_t t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb < 2 || size == 0) return;
    uint8_t *arr = (uint8_t *)base;
    /* Simple shell/insertion hybrid: small code size, no recursion stack risk */
    for (size_t gap = nmemb / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < nmemb; i++) {
            for (size_t j = i; j >= gap; j -= gap) {
                uint8_t *a = arr + (j - gap) * size;
                uint8_t *b = arr + j * size;
                if (compar(a, b) <= 0) break;
                swap_bytes(a, b, size);
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    const uint8_t *arr = (const uint8_t *)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void *elem = arr + mid * size;
        int cmp = compar(key, elem);
        if (cmp == 0) return (void *)(uintptr_t)elem;
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

void abort(void) { uexit(134); }
void exit(int status) { uexit(status); }

/* --- stdio --- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t pos;
} fmt_out_t;

static void fmt_putc(fmt_out_t *o, char c) {
    if (o->buf && o->pos + 1u < o->cap) {
        o->buf[o->pos] = c;
    }
    o->pos++;
}

static void fmt_pad(fmt_out_t *o, char ch, int n) {
    for (int i = 0; i < n; i++) fmt_putc(o, ch);
}

static void fmt_uint(fmt_out_t *o, unsigned long long v, unsigned base, int upper,
                     int neg, int width, int left, char padc) {
    char tmp[32];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v) {
            tmp[n++] = digits[v % base];
            v /= base;
        }
    }
    int total = n + (neg ? 1 : 0);
    int pad = width > total ? (width - total) : 0;
    if (!left && padc == ' ') fmt_pad(o, ' ', pad);
    if (neg) fmt_putc(o, '-');
    if (!left && padc == '0') fmt_pad(o, '0', pad);
    for (int i = n - 1; i >= 0; i--) fmt_putc(o, tmp[i]);
    if (left) fmt_pad(o, ' ', pad);
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    fmt_out_t o = { str, size, 0 };
    const char *p = format;
    while (*p) {
        if (*p != '%') {
            fmt_putc(&o, *p++);
            continue;
        }
        p++;
        int left = 0;
        char padc = ' ';
        while (*p == '-' || *p == '0') {
            if (*p == '-') left = 1;
            else if (*p == '0' && !left) padc = '0';
            p++;
        }
        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
            p++;
        } else {
            while (isdigit((unsigned char)*p)) {
                width = width * 10 + (*p - '0');
                p++;
            }
        }
        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                p++;
            } else {
                while (isdigit((unsigned char)*p)) {
                    prec = prec * 10 + (*p - '0');
                    p++;
                }
            }
        }
        int lmod = 0;
        if (*p == 'l') {
            lmod = 1; p++;
            if (*p == 'l') { lmod = 2; p++; }
        } else if (*p == 'z') {
            lmod = 1; p++;
        }
        char spec = *p ? *p++ : '\0';
        switch (spec) {
            case '%':
                fmt_putc(&o, '%');
                break;
            case 'c': {
                char c = (char)va_arg(ap, int);
                if (!left && width > 1) fmt_pad(&o, ' ', width - 1);
                fmt_putc(&o, c);
                if (left && width > 1) fmt_pad(&o, ' ', width - 1);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                int len = 0;
                while (s[len] && (prec < 0 || len < prec)) len++;
                int pad = width > len ? (width - len) : 0;
                if (!left) fmt_pad(&o, ' ', pad);
                for (int i = 0; i < len; i++) fmt_putc(&o, s[i]);
                if (left) fmt_pad(&o, ' ', pad);
                break;
            }
            case 'd':
            case 'i': {
                long long sv = (lmod == 2) ? va_arg(ap, long long)
                             : (lmod == 1) ? (long long)va_arg(ap, long)
                                           : (long long)va_arg(ap, int);
                int neg = sv < 0;
                unsigned long long uv = neg ? (0ULL - (unsigned long long)sv)
                                            : (unsigned long long)sv;
                fmt_uint(&o, uv, 10, 0, neg, width, left, padc);
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                unsigned long long uv = (lmod == 2) ? va_arg(ap, unsigned long long)
                                      : (lmod == 1) ? (unsigned long long)va_arg(ap, unsigned long)
                                                    : (unsigned long long)va_arg(ap, unsigned int);
                unsigned base = (spec == 'o') ? 8u : (spec == 'u') ? 10u : 16u;
                fmt_uint(&o, uv, base, spec == 'X', 0, width, left, padc);
                break;
            }
            case 'p': {
                uintptr_t pv = (uintptr_t)va_arg(ap, void *);
                fmt_putc(&o, '0');
                fmt_putc(&o, 'x');
                fmt_uint(&o, (unsigned long long)pv, 16, 0, 0, 0, 0, '0');
                break;
            }
            default:
                if (spec) fmt_putc(&o, spec);
                break;
        }
    }
    if (str && size > 0) {
        size_t end = o.pos < size ? o.pos : (size - 1u);
        str[end] = '\0';
    }
    return (int)o.pos;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = vsnprintf(str, size, format, ap);
    va_end(ap);
    return r;
}

int sprintf(char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = vsnprintf(str, (size_t)-1, format, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *format, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), format, ap);
    if (n > 0) uprint(buf);
    return n;
}

int printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = vprintf(format, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE *stream, const char *format, ...) {
    (void)stream;
    va_list ap;
    va_start(ap, format);
    int r = vprintf(format, ap);
    va_end(ap);
    return r;
}

int putchar(int c) {
    uputchar((char)c);
    return (unsigned char)c;
}

int puts(const char *s) {
    uprint(s);
    uputchar('\n');
    return 0;
}

int fputs(const char *s, FILE *stream) {
    (void)stream;
    uprint(s);
    return 0;
}
