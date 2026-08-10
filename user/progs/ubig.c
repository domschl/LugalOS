#include "usys.h"

/* A user program that does not fit the old two-page image (C4,
 * plan/phase6_memory_and_processes.md).
 *
 * Its .bss alone is 20 KB, so with the text page the image spans six pages and
 * is rounded to an eight-page NAPOT run. Under the model this replaces it
 * would not have linked at all: linker/user.ld asserted that .data + .bss fit
 * in one page, because the loader could not allocate anything else.
 *
 * The array is *touched*, first and last byte of every page, rather than
 * merely declared. A .bss that is never written proves nothing: the pages
 * would be granted and the program would run identically whether or not the
 * grant covered them. Writing across the whole span is what makes a missing
 * region a fault instead of a silent pass.
 *
 * Markers:
 *   UBIG_WROTE n   -- how many page-spanning writes landed
 *   UBIG_READBACK  -- and read back as written, so the pages are really there
 *   UBIG_DONE
 */
/* 20 KB, chosen rather than rounded. The image then spans six pages and is
 * rounded up to an eight-page NAPOT run -- so it exercises the padding
 * /proc/meminfo reports, which a program spanning an exact power of two never
 * would. And the data segment, five pages starting at page 1, decomposes into
 * NAPOT pieces of 1 + 2 + 2: three regions, plus one for text and one for the
 * stack, which is exactly MEM_DOMAIN_MAX_REGIONS. It is deliberately at the
 * budget rather than under it. */
#define BIG_BYTES (20 * 1024)
static volatile unsigned char big[BIG_BYTES];

int _start(void) {
    int touched = 0;
    for (int i = 0; i < BIG_BYTES; i += 4096) {
        big[i] = (unsigned char)(i / 4096 + 1);
        big[i + 4095] = (unsigned char)(i / 4096 + 100);
        touched++;
    }

    uprint("UBIG_WROTE ");
    uputnum(touched);
    uprint("\n");

    int ok = 1;
    for (int i = 0; i < BIG_BYTES; i += 4096) {
        if (big[i] != (unsigned char)(i / 4096 + 1)) ok = 0;
        if (big[i + 4095] != (unsigned char)(i / 4096 + 100)) ok = 0;
    }
    if (ok) uprint("UBIG_READBACK\n");

    uprint("UBIG_DONE\n");
    return 0;
}
