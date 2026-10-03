/*
 * `fmtselftest` -- phase 40 review.
 *
 * The kernel's own printf engine (kernel/printk.c) against fixed expected
 * strings, through ksnprintf(), which renders with the same code as printk()
 * and cprintf(). The cases are the ones it got wrong: an unsigned int with its
 * top bit set (printed sign-extended to 64 bits on RV64), LONG_MIN (negated
 * as a signed value -- undefined, and a UBSan halt), and the width, padding
 * and precision forms the rest of the tree relies on. Prints FMTSELFTEST_OK
 * or FMTSELFTEST_FAIL.
 */

#include "kernel/console.h"
#include "kernel/printk.h"

#include <limits.h>
#include <string.h>

static unsigned g_cases, g_wrong;

static void expect(const char *got, const char *want) {
    g_cases++;
    if (strcmp(got, want) != 0) {
        g_wrong++;
        cprintf("fmtselftest: got '%s', want '%s'\n", got, want);
    }
}

void fmt_selftest(void) {
    char b[64];
    g_cases = g_wrong = 0;

    ksnprintf(b, sizeof(b), "%x", 0x80000000u);            expect(b, "80000000");
    ksnprintf(b, sizeof(b), "%08x", 0xdeadbeefu);          expect(b, "deadbeef");
    ksnprintf(b, sizeof(b), "%u", 0xffffffffu);            expect(b, "4294967295");
    ksnprintf(b, sizeof(b), "%d", -1);                     expect(b, "-1");
    ksnprintf(b, sizeof(b), "%d", INT_MIN);                expect(b, "-2147483648");
    ksnprintf(b, sizeof(b), "%5d|%-5d|%05d", 42, 42, -42); expect(b, "   42|42   |-0042");
    ksnprintf(b, sizeof(b), "%lx", 0xfffffffful);          expect(b, "ffffffff");
    ksnprintf(b, sizeof(b), "%.3s|%5s|%-4s|", "abcdef", "ab", "c");
    expect(b, "abc|   ab|c   |");
    ksnprintf(b, sizeof(b), "%c%c%%", 'o', 'k');           expect(b, "ok%");
#if ULONG_MAX > 0xfffffffful
    ksnprintf(b, sizeof(b), "%ld", LONG_MIN);              expect(b, "-9223372036854775808");
    ksnprintf(b, sizeof(b), "%lu", ULONG_MAX);             expect(b, "18446744073709551615");
    ksnprintf(b, sizeof(b), "%lx", 0x123456789abcdef0ul);  expect(b, "123456789abcdef0");
#else
    ksnprintf(b, sizeof(b), "%ld", LONG_MIN);              expect(b, "-2147483648");
    ksnprintf(b, sizeof(b), "%lu", ULONG_MAX);             expect(b, "4294967295");
#endif

    cprintf("fmtselftest: %u cases, %u wrong -- FMTSELFTEST_%s\n",
            g_cases, g_wrong, g_wrong ? "FAIL" : "OK");
}
