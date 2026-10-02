/*
 * `zonetest` -- 38.4, plan/phase38_psram.md §3.1: the bulk page zone, on
 * whatever backs it (PSRAM on the LCD-7, a stand-in range of RAM on QEMU).
 *
 *   1. a bulk allocation lands in the bulk zone, a fast one does not;
 *   2. NAPOT alignment holds in the bulk zone (PMP checks addresses only);
 *   3. palloc_free() finds the zone from the address: both zones' free
 *      counts come back exactly;
 *   4. a re-allocated page reads zero through the normal (cached) address
 *      even when its previous owner left dirty cache lines in it -- the
 *      case the invalidate-then-zero-uncached path exists for;
 *   5. with the bulk zone exhausted, a bulk request is served from SRAM and
 *      counted as a fallback;
 *   6. a BULK_BSS object lies in PSRAM (on a board with one), is zero at
 *      boot, and holds what is written.
 *
 * Takes every free bulk page for a moment (step 5), so anything else asking
 * for bulk memory meanwhile falls back too; a test command, not a background
 * check. Prints ZONETEST_OK, ZONETEST_FAIL, or ZONETEST_SKIP when this build
 * has no bulk zone.
 */

#include "kernel/console.h"
#include "kernel/palloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_CHUNKS 64

#if defined(CONFIG_PALLOC_BULK_PAGES)
/* 6. A BULK_BSS object: placed in PSRAM on a board that has one, zero at
 * boot (psram_init() zeroes the section; the reset handler runs before the
 * chip answers). 64 bytes, and only on builds with a bulk zone. */
static volatile uint32_t g_bulk_probe[16] BULK_BSS;
static bool g_probe_seen;
#endif

static unsigned g_bad;

static void check(int ok, const char *what) {
    if (!ok) { g_bad++; cprintf("  FAIL: %s\n", what); }
}

void zone_selftest(void) {
    palloc_zone_stats_t z0, z;
    if (!palloc_bulk_stats(&z0)) {
        cprintf("zonetest: this build has no bulk zone -- ZONETEST_SKIP\n");
        return;
    }
    uint32_t fast_total, fast_free0, fast_free;
    palloc_stats(&fast_total, &fast_free0);
    g_bad = 0;

    /* 1. Which zone. */
    uint8_t *b = palloc_pages_bulk(2);
    uint8_t *f = palloc_pages(1);
    check(b && palloc_is_bulk(b), "a bulk allocation is in the bulk zone");
    check(f && !palloc_is_bulk(f), "a fast allocation is not");

    /* 2. NAPOT: 4 pages on a 16 KB boundary. */
    uint8_t *n = palloc_pages_bulk_aligned(4, 4);
    check(n && palloc_is_bulk(n) && ((uintptr_t)n & (4u * PAGE_SIZE - 1)) == 0,
          "an aligned bulk allocation is in the zone and self-aligned");

    /* 4. Dirty lines must not survive into the next owner. */
    if (b) {
        volatile uint32_t *w = (volatile uint32_t *)b;
        for (uint32_t i = 0; i < 2u * PAGE_SIZE / 4; i++) w[i] = 0xC0FFEE00u | i;
        palloc_free(b, 2);
        uint8_t *again = palloc_pages_bulk(2);
        check(again == b, "the freed run is handed out again (first fit)");
        uint32_t nonzero = 0;
        if (again) {
            volatile uint32_t *r = (volatile uint32_t *)again;
            for (uint32_t i = 0; i < 2u * PAGE_SIZE / 4; i++) nonzero += r[i] != 0;
        }
        check(nonzero == 0, "a re-allocated bulk run reads zero through the cached address");
        b = again;
    }

    /* 3. Free by address, both zones. */
    if (b) palloc_free(b, 2);
    if (n) palloc_free(n, 4);
    if (f) palloc_free(f, 1);
    palloc_bulk_stats(&z);
    palloc_stats(NULL, &fast_free);
    check(z.free_pages == z0.free_pages, "the bulk zone's free count comes back");
    check(fast_free == fast_free0, "the fast zone's free count comes back");

    /* 5. Exhaust the bulk zone, largest run first, then ask once more. */
    void *chunk[MAX_CHUNKS];
    uint32_t size[MAX_CHUNKS], chunks = 0;
    while (chunks < MAX_CHUNKS && palloc_bulk_stats(&z) && z.free_pages > 0) {
        uint32_t want = z.largest_free_run;
        void *p = palloc_pages_bulk(want);
        if (!p || !palloc_is_bulk(p)) { if (p) palloc_free(p, want); break; }
        chunk[chunks] = p;
        size[chunks++] = want;
    }
    palloc_bulk_stats(&z);
    check(z.free_pages == 0, "the bulk zone could be emptied");
    uint32_t fallbacks_before = z.fallbacks;
    void *spill = palloc_pages_bulk(1);
    palloc_bulk_stats(&z);
    check(spill && !palloc_is_bulk(spill), "an exhausted bulk zone falls back to SRAM");
    check(z.fallbacks == fallbacks_before + 1, "the fallback is counted");
    if (spill) palloc_free(spill, 1);
    for (uint32_t i = 0; i < chunks; i++) palloc_free(chunk[i], size[i]);

    palloc_bulk_stats(&z);
    palloc_stats(NULL, &fast_free);
    check(z.free_pages == z0.free_pages && fast_free == fast_free0, "everything was given back");

#if defined(CONFIG_PALLOC_BULK_PAGES)
    /* 6. BULK_BSS: where it is, and zero until written. Only the first run
     * since boot can check the zeroing; later runs find their own pattern
     * cleared again below. */
#if defined(CONFIG_PSRAM_BYTES)
    check((uintptr_t)g_bulk_probe >= 0x11000000u && (uintptr_t)g_bulk_probe < 0x12000000u,
          "a BULK_BSS object is in the PSRAM window");
#endif
    uint32_t nz = 0;
    for (unsigned i = 0; i < 16; i++) nz += g_bulk_probe[i] != 0;
    check(nz == 0, g_probe_seen ? "BULK_BSS probe cleared by the previous run"
                                : "a BULK_BSS object is zero at boot");
    for (unsigned i = 0; i < 16; i++) g_bulk_probe[i] = 0xB0000000u | i;
    for (unsigned i = 0; i < 16; i++) nz += g_bulk_probe[i] != (0xB0000000u | i);
    check(nz == 0, "a BULK_BSS object holds what is written");
    for (unsigned i = 0; i < 16; i++) g_bulk_probe[i] = 0;
    g_probe_seen = true;
    cprintf("zonetest: BULK_BSS probe at 0x%08lx\n", (unsigned long)(uintptr_t)g_bulk_probe);
#endif
    cprintf("zonetest: bulk zone %lu pages at 0x%08lx (%lu free), %lu chunks to exhaust it, "
            "fallbacks %lu -- ZONETEST_%s\n",
            (unsigned long)z.total_pages, (unsigned long)z.base, (unsigned long)z.free_pages,
            (unsigned long)chunks, (unsigned long)z.fallbacks, g_bad ? "FAIL" : "OK");
}
