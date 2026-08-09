#include "usys.h"

/* A user program, linked separately from the kernel and run in U-mode under a
 * memory domain that grants it three pages and nothing else.
 *
 * It is written to *use* each part of the image model rather than merely to
 * print something, because the parts are what could quietly be broken:
 *
 *   - a string literal, which lives in .rodata in the program's own text page
 *     (in the kernel's `.utext` probes this is the thing that cannot be done,
 *     and every message there is assembled one character at a time to avoid
 *     it);
 *   - a writable global, which lives in .bss on the second page and would
 *     fault if that page were not mapped R|W;
 *   - a pointer-taking syscall reading into a buffer on that same page, which
 *     the kernel must validate against this task's domain and copy out to.
 *
 * Each step prints a distinct marker, so a test can tell *which* of them
 * failed rather than only that the program did not finish.
 */

static const char banner[] = "UPROG_TEXT_OK hello from a separately linked U-mode program\n";

/* .bss: the second image page. Writing these at all proves it is mapped R|W. */
static long  run_count;
static char  scratch[192];

static int is_printable(char c) {
    return c >= 0x20 && c < 0x7f;
}

int _start(void) {
    /* .rodata, addressed PC-relatively out of this program's own text page. */
    uprint(banner);

    /* A writable global. If page 1 were missing or read-only this store
     * faults and the kernel terminates the task -- which is why the marker is
     * printed after the store, not before. */
    run_count++;
    uprint("UPROG_DATA_OK wrote a global\n");
    uprint("UPROG_RUNS ");
    uputnum(run_count);
    uputchar('\n');

    /* A syscall that hands the kernel two pointers: a path in .rodata (which
     * the kernel must be able to read) and a destination in .bss (which it
     * must be able to write). Both are inside this task's domain, so both are
     * permitted; the same call with a kernel address would be refused, which
     * is what kernel/shell.c's `deputytest` asserts. */
    for (unsigned i = 0; i < sizeof(scratch); i++) scratch[i] = 0;
    long n = uread_file("/proc/version", scratch, (long)sizeof(scratch) - 1);
    if (n < 0) {
        uprint("UPROG_FILE_FAILED\n");
        return 1;
    }
    scratch[n] = '\0';

    /* Print the first line of what came back, so the marker is evidence that
     * real bytes crossed the boundary rather than that a syscall returned a
     * non-negative number. */
    uprint("UPROG_FILE_OK ");
    for (long i = 0; i < n && scratch[i] != '\n'; i++) {
        if (is_printable(scratch[i])) uputchar(scratch[i]);
    }
    uputchar('\n');

    /* Returning is the ordinary way for a program to end: the loader put an
     * exit stub in ra, which turns this value into the task's exit status.
     * `exec` prints it, so the number below is checked by the test. */
    return 7;
}
