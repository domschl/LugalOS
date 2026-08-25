#include "string.h"

void *memcpy(void *dst, const void *src, size_t n) {
    if (!dst || !src) return dst;
    char *d = (char *)dst;
    const char *s = (const char *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

/* The direction is the whole function: copying forwards when the destination
 * overlaps the tail of the source overwrites bytes that have not been read
 * yet, so that case walks backwards instead. */
void *memmove(void *dst, const void *src, size_t n) {
    if (!dst || !src || dst == src) return dst;
    char *d = (char *)dst;
    const char *s = (const char *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

void *memset(void *s, int c, size_t n) {
    if (!s) return s;
    unsigned char *p = (unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (unsigned char)c;
    }
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

