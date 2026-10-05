#ifndef LUGALOS_RADIO_UPRINTF_H
#define LUGALOS_RADIO_UPRINTF_H

#include <stdarg.h>
#include <stdint.h>

/* A printf for confined U-mode code (45.3b, plan/phase45_esp32c6.md).
 *
 * The Wi-Fi blob logs through its OS table with a C format string and a
 * va_list. The kernel's own formatter cannot be reached from U-mode, and the
 * blob's logging is how a radio that does not come up is diagnosed, so the radio
 * domain carries a formatter of its own: no libc, no allocation, no static
 * state, and no 64-bit division (a U-mode text page cannot call libgcc's
 * __udivdi3, and this must link into one).
 *
 * Supports  %% %c %s %d %i %u %x %X %o %p  with the flags `-` `0` `+` ` ` `#`,
 * a width and precision (digits or `*`), and the length modifiers hh h l ll z.
 * Floating-point conversions consume their argument and print `?`: the blob
 * logs integers and strings, and soft-float formatting is a lot of code for a
 * thing nobody has seen it need.
 *
 * Output is truncated to `cap - 1` bytes and always NUL-terminated (cap > 0).
 * Returns the length it *would* have written, like snprintf. */
int ku_vsnprintf(char *buf, uint32_t cap, const char *fmt, va_list ap);
int ku_snprintf(char *buf, uint32_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#endif
