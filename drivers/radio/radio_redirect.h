#ifndef LUGALOS_RADIO_REDIRECT_H
#define LUGALOS_RADIO_REDIRECT_H

#include <stddef.h>
#include <stdarg.h>

/* Compiler-generated calls (a struct copied by value, a large initialiser, a
 * zeroed array) go to `memcpy`/`memset` by name, whatever the source says --
 * and in this image those names are the kernel's, in kernel text, which the
 * radio's U-mode domain cannot execute. Declaring them with an assembler label
 * sends the compiler's own calls to the radio's copies instead: `radio_memcpy`
 * (drivers/radio/radio_libc.c) and, for memset, the ROM's (the address
 * tools/c6_radio_libs.py assigns). Include this first in every radio source
 * that is not the shim itself (the supplicant's are compiled with -include);
 * tools/check_radio_text.py checks the result in the object code. */
void *memcpy(void *, const void *, size_t) __asm__("radio_memcpy");
void *memmove(void *, const void *, size_t) __asm__("radio_memmove");
void *memset(void *, int, size_t) __asm__("radio_memset");
int memcmp(const void *, const void *, size_t) __asm__("radio_memcmp");
size_t strlen(const char *) __asm__("radio_strlen");
int strcmp(const char *, const char *) __asm__("radio_strcmp");
int strncmp(const char *, const char *, size_t) __asm__("radio_strncmp");
char *strchr(const char *, int) __asm__("radio_strchr");
char *strrchr(const char *, int) __asm__("radio_strrchr");
char *strstr(const char *, const char *) __asm__("radio_strstr");
char *strcpy(char *, const char *) __asm__("radio_strcpy");
char *strncpy(char *, const char *, size_t) __asm__("radio_strncpy");
long strtol(const char *, char **, int) __asm__("radio_strtol");
int atoi(const char *) __asm__("radio_atoi");
void *malloc(size_t) __asm__("radio_malloc");
void *calloc(size_t, size_t) __asm__("radio_calloc");
void *realloc(void *, size_t) __asm__("radio_realloc");
void free(void *) __asm__("radio_free");
int snprintf(char *, size_t, const char *, ...) __asm__("radio_snprintf");
int vsnprintf(char *, size_t, const char *, va_list) __asm__("radio_vsnprintf");
void abort(void) __asm__("radio_abort");
#define bzero(p, n) memset((p), 0, (n))

#endif
