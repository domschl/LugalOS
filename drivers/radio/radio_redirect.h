#ifndef LUGALOS_RADIO_REDIRECT_H
#define LUGALOS_RADIO_REDIRECT_H

#include <stddef.h>

/* Compiler-generated calls (a struct copied by value, a large initialiser, a
 * zeroed array) go to `memcpy`/`memset` by name, whatever the source says --
 * and in this image those names are the kernel's, in kernel text, which the
 * radio's U-mode domain cannot execute. Declaring them with an assembler label
 * sends the compiler's own calls to the radio's copies instead: `radio_memcpy`
 * (drivers/radio/radio_libc.c) and, for memset, the ROM's (the address
 * tools/c6_radio_libs.py assigns). Include this first in every radio source
 * that is not the shim itself; tools/check_radio_text.py checks the result in
 * the object code. */
void *memcpy(void *, const void *, size_t) __asm__("radio_memcpy");
void *memmove(void *, const void *, size_t) __asm__("radio_memmove");
void *memset(void *, int, size_t) __asm__("radio_memset");
int memcmp(const void *, const void *, size_t) __asm__("radio_memcmp");

#endif
