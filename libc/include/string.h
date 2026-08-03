#ifndef LUGALOS_LIBC_STRING_H
#define LUGALOS_LIBC_STRING_H

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);

char *strchr(const char *s, int c);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
char *strstr(const char *haystack, const char *needle);

#endif /* LUGALOS_LIBC_STRING_H */

