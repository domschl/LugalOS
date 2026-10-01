#ifndef LUGALOS_KERNEL_CLIPBOARD_H
#define LUGALOS_KERNEL_CLIPBOARD_H

#include <stdbool.h>
#include <stdint.h>

/* The clipboard -- 37.5a, plan/phase37_screen_layouts_and_apps.md.
 *
 * One, for the whole machine: what the line editor cuts, `e` pastes, Lisp
 * reads and a 9P client sees as /dev/clipboard (Plan 9's /dev/snarf). Up to
 * CLIPBOARD_MAX bytes of text, held in heap pages taken on first use and
 * kept from then on. Not locked: text is put in and taken out by whoever is
 * at the keyboard, one at a time. */

#define CLIPBOARD_MAX 8192u

/* Replaces the contents; false (contents unchanged) if `n` is over the
 * limit or no memory could be had. */
bool clipboard_set(const char *s, uint32_t n);

/* `n` bytes at `offset` -- at 0 the contents are replaced, at the current
 * end they are appended (a file written in pieces), anywhere else refused.
 * Returns the bytes taken, or -1. */
int clipboard_write_at(const char *s, uint32_t n, uint32_t offset);

/* The contents, `*n` bytes, not NUL-terminated; NULL with *n = 0 if empty. */
const char *clipboard_data(uint32_t *n);

#endif /* LUGALOS_KERNEL_CLIPBOARD_H */
