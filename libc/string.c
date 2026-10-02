#include "string.h"

/* 38.3, plan/phase38_psram.md: word-wide copies and fills.
 *
 * These were byte loops, which cost a 46 KB text scroll 2.0 ms against 693 us
 * as words (phase 36, 36.5) and copied out of PSRAM at 9.6 MB/s where the bus
 * gives ~22 (plan/phase38_preliminaries.md §2). Words are used when the two
 * pointers share their alignment within a word -- then a byte head brings
 * both to a word boundary at once, and a byte tail finishes. Pointers that
 * differ in alignment stay on the byte loop: the RP2350's Hazard3 traps on a
 * misaligned word access, and shifting words into place is more code than
 * the cases that hit it are worth.
 *
 * `word_t` is the machine word (8 bytes on RV64) and is may_alias: these
 * functions are handed any type's memory, and strict aliasing would let the
 * compiler reorder word accesses against the caller's own. Four words are
 * loaded before any is stored, which is what lets memmove's forward case
 * share the same loop when the destination overlaps below the source.
 *
 * This file is built with -fno-tree-loop-distribute-patterns (CMakeLists.txt):
 * otherwise GCC may recognise these loops as the very functions they are and
 * turn them into calls to themselves. */
typedef unsigned long __attribute__((may_alias)) word_t;
#define WSIZE ((size_t)sizeof(word_t))
#define WMASK ((uintptr_t)(WSIZE - 1))

static void copy_forward(unsigned char *d, const unsigned char *s, size_t n) {
    if ((((uintptr_t)d ^ (uintptr_t)s) & WMASK) == 0) {
        while (n && ((uintptr_t)d & WMASK)) { *d++ = *s++; n--; }
        word_t *dw = (word_t *)(void *)d;
        const word_t *sw = (const word_t *)(const void *)s;
        while (n >= 4 * WSIZE) {
            word_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
            dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
            dw += 4; sw += 4; n -= 4 * WSIZE;
        }
        while (n >= WSIZE) { *dw++ = *sw++; n -= WSIZE; }
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while (n--) *d++ = *s++;
}

/* From the end down: the destination overlaps above the source. */
static void copy_backward(unsigned char *d, const unsigned char *s, size_t n) {
    d += n;
    s += n;
    if ((((uintptr_t)d ^ (uintptr_t)s) & WMASK) == 0) {
        while (n && ((uintptr_t)d & WMASK)) { *--d = *--s; n--; }
        word_t *dw = (word_t *)(void *)d;
        const word_t *sw = (const word_t *)(const void *)s;
        while (n >= 4 * WSIZE) {
            dw -= 4; sw -= 4;
            word_t a = sw[3], b = sw[2], c = sw[1], e = sw[0];
            dw[3] = a; dw[2] = b; dw[1] = c; dw[0] = e;
            n -= 4 * WSIZE;
        }
        while (n >= WSIZE) { *--dw = *--sw; n -= WSIZE; }
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while (n--) *--d = *--s;
}

void *memcpy(void *dst, const void *src, size_t n) {
    if (!dst || !src) return dst;
    copy_forward((unsigned char *)dst, (const unsigned char *)src, n);
    return dst;
}

/* The direction is the whole function: copying forwards when the destination
 * overlaps the tail of the source overwrites bytes that have not been read
 * yet, so that case walks backwards instead. */
void *memmove(void *dst, const void *src, size_t n) {
    if (!dst || !src || dst == src) return dst;
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if ((uintptr_t)d < (uintptr_t)s || (uintptr_t)d >= (uintptr_t)s + n) {
        copy_forward(d, s, n);
    } else {
        copy_backward(d, s, n);
    }
    return dst;
}

void *memset(void *s, int c, size_t n) {
    if (!s) return s;
    unsigned char *p = (unsigned char *)s;
    unsigned char b = (unsigned char)c;
    while (n && ((uintptr_t)p & WMASK)) { *p++ = b; n--; }
    if (n >= WSIZE) {
        word_t w = (word_t)b * (word_t)(~(word_t)0 / 0xffu);   /* b in every byte */
        word_t *pw = (word_t *)(void *)p;
        while (n >= 4 * WSIZE) {
            pw[0] = w; pw[1] = w; pw[2] = w; pw[3] = w;
            pw += 4; n -= 4 * WSIZE;
        }
        while (n >= WSIZE) { *pw++ = w; n -= WSIZE; }
        p = (unsigned char *)pw;
    }
    while (n--) *p++ = b;
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

int strcmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0' || s2[i] == '\0') {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    if (!dst) return NULL;
    if (!src) {
        dst[0] = '\0';
        return dst;
    }
    char *orig = dst;
    while ((*dst++ = *src++) != '\0');
    return orig;
}

char *strncpy(char *dst, const char *src, size_t n) {

    if (!dst) return NULL;
    if (!src) {
        if (n > 0) dst[0] = '\0';
        return dst;
    }
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

char *strchr(const char *s, int c) {
    if (!s) return NULL;
    while (*s != '\0') {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strcat(char *dst, const char *src) {
    if (!dst || !src) return dst;
    char *p = dst + strlen(dst);
    while ((*p++ = *src++) != '\0');
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    if (!dst || !src || n == 0) return dst;
    char *p = dst + strlen(dst);
    size_t i = 0;
    while (i < n && src[i] != '\0') {
        p[i] = src[i];
        i++;
    }
    p[i] = '\0';
    return dst;
}

char *strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (*needle == '\0') return (char *)haystack;
    size_t needle_len = strlen(needle);
    while (*haystack != '\0') {
        if (strncmp(haystack, needle, needle_len) == 0) {
            return (char *)haystack;
        }
        haystack++;
    }
    return NULL;
}

