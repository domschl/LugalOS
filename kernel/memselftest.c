/*
 * `memselftest` -- 38.3, plan/phase38_psram.md.
 *
 * libc's memcpy/memmove/memset became word loops with byte heads and tails,
 * and everything links against them, so they are checked exhaustively over
 * the cases where a word loop goes wrong: every source and destination offset
 * within two words, every length up to 67 (head, whole words, four-word
 * blocks and tail in every combination), and memmove in both overlap
 * directions at every distance -- each against a byte-at-a-time reference,
 * with guard bytes either side that must not change. Then one large copy of
 * each kind. Prints MEMSELFTEST_OK or MEMSELFTEST_FAIL.
 */

#include "kernel/console.h"
#include "kernel/palloc.h"

#include <stdint.h>
#include <string.h>

#define WS      ((unsigned)sizeof(unsigned long))   /* 4 on RV32, 8 on RV64 */
#define OFFS    (2u * WS)
#define MAXLEN  67u
#define AREA    256u                                /* > 2 * (OFFS + MAXLEN) */
#define GUARD   0xA5u

static void fill(unsigned char *p, unsigned n, unsigned seed) {
    for (unsigned i = 0; i < n; i++) p[i] = (unsigned char)(i * 7u + seed * 13u + 1u);
}

static int same(const unsigned char *a, const unsigned char *b, unsigned n) {
    for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void ref_move(unsigned char *d, const unsigned char *s, unsigned n) {
    if (d < s) { for (unsigned i = 0; i < n; i++) d[i] = s[i]; }
    else       { for (unsigned i = n; i > 0; i--) d[i - 1] = s[i - 1]; }
}

void mem_selftest(void) {
    unsigned char *page = palloc_pages(2);
    if (!page) { cprintf("memselftest: no pages -- MEMSELFTEST_FAIL\n"); return; }
    unsigned char *a = page, *b = page + AREA, *r = page + 2 * AREA, *src = page + 3 * AREA;
    unsigned cases = 0, bad = 0;

    /* memcpy and memset: separate buffers, every offset pair and length. */
    for (unsigned so = 0; so < OFFS; so++) {
        for (unsigned d0 = 0; d0 < OFFS; d0++) {
            for (unsigned n = 0; n <= MAXLEN; n++) {
                fill(src, AREA, so + n);
                memset(a, GUARD, AREA);
                memset(r, GUARD, AREA);       /* memset itself is checked below */
                for (unsigned i = 0; i < n; i++) r[d0 + i] = src[so + i];
                memcpy(a + d0, src + so, n);
                cases++;
                if (!same(a, r, AREA)) { if (bad++ < 4) cprintf("  memcpy so=%u do=%u n=%u\n", so, d0, n); }
            }
        }
    }
    for (unsigned d0 = 0; d0 < OFFS; d0++) {
        for (unsigned n = 0; n <= MAXLEN; n++) {
            for (unsigned i = 0; i < AREA; i++) { a[i] = GUARD; r[i] = GUARD; }
            for (unsigned i = 0; i < n; i++) r[d0 + i] = (unsigned char)(0x3Cu + n);
            memset(a + d0, (int)(0x3Cu + n) | 0x100, n);   /* only the low byte counts */
            cases++;
            if (!same(a, r, AREA)) { if (bad++ < 4) cprintf("  memset do=%u n=%u\n", d0, n); }
        }
    }

    /* memmove within one buffer: source and destination at every pair of
     * offsets, so both overlap directions and every distance occur. */
    for (unsigned so = 0; so < OFFS + 16u; so++) {
        for (unsigned d0 = 0; d0 < OFFS + 16u; d0++) {
            for (unsigned n = 0; n <= MAXLEN; n++) {
                fill(a, AREA, so * 3u + d0 + n);
                for (unsigned i = 0; i < AREA; i++) b[i] = a[i];
                ref_move(b + d0, b + so, n);
                memmove(a + d0, a + so, n);
                cases++;
                if (!same(a, b, AREA)) { if (bad++ < 4) cprintf("  memmove so=%u do=%u n=%u\n", so, d0, n); }
            }
        }
    }

    /* One large of each, over two pages: 4000 bytes at an odd offset. */
    unsigned char *big = page;
    const unsigned L = 4000u;
    fill(big + 4096, 4096, 9);
    memcpy(big + 3, big + 4096 + 3, L);
    cases++;
    if (!same(big + 3, big + 4096 + 3, L)) { bad++; cprintf("  memcpy large\n"); }
    for (unsigned i = 0; i < 8192; i++) big[i] = (unsigned char)(i * 5u);
    memmove(big + 101, big + 100, L);               /* overlapping, upwards by one */
    cases++;
    for (unsigned i = 0; i < L; i++) {
        if (big[101 + i] != (unsigned char)((100 + i) * 5u)) { bad++; cprintf("  memmove large up\n"); break; }
    }
    for (unsigned i = 0; i < 8192; i++) big[i] = (unsigned char)(i * 5u);
    memmove(big + 100, big + 108, L);               /* overlapping, downwards by two words */
    cases++;
    for (unsigned i = 0; i < L; i++) {
        if (big[100 + i] != (unsigned char)((108 + i) * 5u)) { bad++; cprintf("  memmove large down\n"); break; }
    }

    palloc_free(page, 2);
    cprintf("memselftest: %u cases, %u wrong, word %u bytes -- MEMSELFTEST_%s\n",
            cases, bad, WS, bad ? "FAIL" : "OK");
}
