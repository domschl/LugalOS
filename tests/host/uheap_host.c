/*
 * kernel/uheap.c on the host, under ASan/UBSan or valgrind (45.3b,
 * plan/phase45_esp32c6.md).
 *
 * The radio's heap lives in memory the blob can overrun, so what has to hold is
 * (a) the allocator is correct when its client is, and (b) uheap_check()
 * notices when it is not. Both are tested:
 *
 *   * a randomized run against a model: allocations never overlap and stay
 *     inside the arena, 8-byte aligned; their contents survive every other
 *     operation (a per-allocation byte pattern); realloc keeps the prefix;
 *     calloc is zeroed; a double free and a pointer from nowhere are ignored;
 *     uheap_check() is clean throughout; and when everything is freed the heap
 *     is one block again, the size it started;
 *   * deliberate corruption -- a header overwritten, a size that runs off the
 *     end, a free-list link redirected -- each of which uheap_check() must
 *     report.
 *
 * ASan sees the arena as a plain heap object, so an access one byte past it is
 * caught here even though the allocator itself would not notice.
 *
 * Usage: uheap_host [iterations [seed]].
 */

#include "kernel/uheap.h"
#include "shim.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); abort(); } } while (0)

#define ARENA 65536u

static uint64_t g_rs;
static uint32_t rnd(uint32_t n) { return host_rand(&g_rs) % n; }

typedef struct { uint8_t *p; uint32_t n; uint8_t seed; } live_t;

static void fill(const live_t *a) { for (uint32_t i = 0; i < a->n; i++) a->p[i] = (uint8_t)(a->seed + i * 7u); }
static bool intact(const live_t *a, uint32_t upto) {
    for (uint32_t i = 0; i < upto; i++) if (a->p[i] != (uint8_t)(a->seed + i * 7u)) return false;
    return true;
}

static void test_basics(void) {
    uint64_t *mem = aligned_alloc(8, ARENA);
    uheap_t h;
    CHECK(!uheap_init(&h, mem, 32), "a 32-byte arena is refused");
    CHECK(!uheap_init(&h, (uint8_t *)mem + 4, 4096), "a misaligned arena is refused");
    CHECK(uheap_init(&h, mem, ARENA), "init");
    uint32_t whole = uheap_largest_free(&h);
    CHECK(whole == ARENA - 8 - 8, "a fresh heap's largest block is the arena less a header and the sentinel: %u", whole);
    CHECK(uheap_check(&h) == 0, "fresh heap is sound");

    CHECK(uheap_alloc(&h, 0) == NULL, "alloc(0)");
    CHECK(uheap_alloc(&h, ARENA) == NULL && h.fail_count == 1, "alloc larger than the arena");
    uint8_t *a = uheap_alloc(&h, 1);
    CHECK(a && ((uintptr_t)a & 7) == 0, "a 1-byte allocation is 8-aligned");
    uint8_t *b = uheap_alloc(&h, 100);
    uint8_t *c = uheap_alloc(&h, 100);
    CHECK(b > a && c > b + 100, "allocations are laid out in order and apart");
    uheap_free(&h, b);
    uheap_free(&h, b);                              /* a double free */
    CHECK(uheap_check(&h) == 0, "a double free is ignored, not acted on");
    uheap_free(&h, (uint8_t *)mem + 8);             /* inside the arena, not an allocation */
    uheap_free(&h, (void *)&h);                     /* nowhere near */
    uheap_free(&h, NULL);
    CHECK(uheap_check(&h) == 0, "forged and null frees are ignored");

    uheap_free(&h, a); uheap_free(&h, c);
    CHECK(uheap_largest_free(&h) == whole, "everything freed coalesces back into one block");
    CHECK(h.used_bytes == 0 && h.used_blocks == 0, "accounting returns to zero");

    /* calloc: zeroed, and refuses an overflowing product. */
    memset(mem, 0xAA, ARENA);
    uheap_init(&h, mem, ARENA);
    uint8_t *z = uheap_calloc(&h, 10, 37);
    int zero = 1; for (int i = 0; i < 370; i++) if (z[i]) zero = 0;
    CHECK(z && zero, "calloc zeroes");
    CHECK(uheap_calloc(&h, 0x10000u, 0x10001u) == NULL, "calloc overflow refused");

    /* realloc: NULL allocates, 0 frees, growth keeps the prefix, failure keeps the original. */
    CHECK(uheap_realloc(&h, NULL, 50) != NULL, "realloc(NULL, n)");
    uint8_t *r = uheap_alloc(&h, 40);
    for (int i = 0; i < 40; i++) r[i] = (uint8_t)i;
    uint8_t *r2 = uheap_realloc(&h, r, 3000);
    int keep = 1; for (int i = 0; i < 40; i++) if (r2[i] != i) keep = 0;
    CHECK(r2 && keep, "grown realloc keeps the old contents");
    CHECK(uheap_realloc(&h, r2, ARENA) == NULL, "an impossible realloc fails");
    keep = 1; for (int i = 0; i < 40; i++) if (r2[i] != i) keep = 0;
    CHECK(keep && uheap_check(&h) == 0, "...and leaves the original block intact");
    CHECK(uheap_realloc(&h, r2, 0) == NULL, "realloc(p, 0) frees");
    CHECK(uheap_check(&h) == 0, "sound after the realloc sequence");
    free(mem);
}

/* The pattern the radio produces: many same-sized buffers, freed out of order. */
static void test_churn(void) {
    uint64_t *mem = aligned_alloc(8, ARENA);
    uheap_t h; uheap_init(&h, mem, ARENA);
    uint8_t *p[32]; int n = 0;
    while (n < 32 && (p[n] = uheap_alloc(&h, 1700)) != NULL) n++;
    CHECK(n >= 30, "a 64 KB arena holds ~37 1.7 KB buffers; got %d", n);
    for (int i = 0; i < n; i += 2) uheap_free(&h, p[i]);
    CHECK(uheap_check(&h) == 0, "sound with every other buffer freed");
    int again = 0;
    for (int i = 0; i < n; i += 2) { p[i] = uheap_alloc(&h, 1700); if (p[i]) again++; }
    CHECK(again == (n + 1) / 2, "the freed holes take the same-sized buffers again (%d of %d)", again, (n + 1) / 2);
    for (int i = 0; i < n; i++) uheap_free(&h, p[i]);
    CHECK(uheap_largest_free(&h) == ARENA - 16 && uheap_check(&h) == 0, "all freed: one block");
    free(mem);
}

static void test_corruption_is_noticed(void) {
    uint64_t *mem = aligned_alloc(8, ARENA);
    uheap_t h;
    uint8_t *p[6];
    for (int trial = 0; trial < 5; trial++) {
        uheap_init(&h, mem, ARENA);
        for (int i = 0; i < 6; i++) p[i] = uheap_alloc(&h, 200);
        uheap_free(&h, p[1]); uheap_free(&h, p[3]);
        CHECK(uheap_check(&h) == 0, "baseline sound");
        uint32_t *hdr2 = (uint32_t *)(p[2] - 8);
        switch (trial) {
        case 0: hdr2[0] = 0x00ffff01u; break;                       /* a size that runs off the arena */
        case 1: hdr2[1] ^= 0x40; break;                              /* a back-pointer that disagrees */
        case 2: hdr2[0] = 12 | 1; break;                             /* a size below the minimum and not 8-aligned */
        case 3: { uint32_t *fr = (uint32_t *)(p[1] - 8); fr[2] = 0x7fffffff; break; }   /* a redirected free-list link */
        case 4: h.used_bytes += 8; break;                            /* accounting that disagrees with the heap */
        }
        CHECK(uheap_check(&h) > 0, "trial %d: corruption went unreported", trial);
    }
    free(mem);
}

static void test_random(int iters) {
    uint64_t *mem = aligned_alloc(8, ARENA);
    uheap_t h; uheap_init(&h, mem, ARENA);
    live_t live[256]; int nl = 0;
    uint8_t seed = 1;

    for (int it = 0; it < iters; it++) {
        int op = (int)rnd(10);
        if (op < 4 || nl == 0) {                                     /* alloc */
            if (nl == 256) continue;
            uint32_t n = rnd(5) == 0 ? 1 + rnd(4000) : 1 + rnd(200);
            uint8_t *p = rnd(8) == 0 ? uheap_calloc(&h, 1, n) : uheap_alloc(&h, n);
            if (!p) continue;
            CHECK(((uintptr_t)p & 7) == 0, "alignment");
            CHECK((uint8_t *)p >= (uint8_t *)mem + 8 && (uint8_t *)p + n <= (uint8_t *)mem + ARENA - 8, "outside the arena");
            live[nl] = (live_t){ p, n, seed++ };
            fill(&live[nl]);
            nl++;
        } else if (op < 7) {                                         /* free */
            int i = (int)rnd((uint32_t)nl);
            CHECK(intact(&live[i], live[i].n), "contents of a live block were damaged");
            uheap_free(&h, live[i].p);
            live[i] = live[--nl];
        } else if (op < 9) {                                         /* realloc */
            int i = (int)rnd((uint32_t)nl);
            uint32_t n = 1 + rnd(3000);
            uint32_t keep = n < live[i].n ? n : live[i].n;
            uint8_t *q = uheap_realloc(&h, live[i].p, n);
            if (!q) { CHECK(intact(&live[i], live[i].n), "a failed realloc damaged the original"); continue; }
            live[i].p = q;
            CHECK(intact(&live[i], keep), "realloc lost the prefix");
            live[i].n = n; fill(&live[i]);
        } else {                                                     /* a bad free */
            uheap_free(&h, (uint8_t *)mem + 8 * (1 + rnd(ARENA / 8 - 2)) + 1);   /* misaligned: ignored */
        }
        if ((it & 63) == 0) {
            CHECK(uheap_check(&h) == 0, "uheap_check at step %d", it);
            for (int i = 0; i < nl; i++)
                for (int j = i + 1; j < nl; j++)
                    CHECK(live[i].p + live[i].n <= live[j].p || live[j].p + live[j].n <= live[i].p,
                          "allocations %d and %d overlap", i, j);
        }
    }
    for (int i = 0; i < nl; i++) { CHECK(intact(&live[i], live[i].n), "final contents"); uheap_free(&h, live[i].p); }
    CHECK(uheap_check(&h) == 0, "sound at the end");
    CHECK(uheap_largest_free(&h) == ARENA - 16, "everything freed: one block of %u, got %u", ARENA - 16, uheap_largest_free(&h));
    CHECK(h.used_bytes == 0 && h.used_blocks == 0, "accounting back to zero");
    free(mem);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 2000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x40c0ffee;
    printf("uheap_host: seed %#llx, %d iterations\n", (unsigned long long)seed, iterations);
    test_basics();
    test_churn();
    test_corruption_is_noticed();
    printf("uheap_host: scenarios pass\n");
    g_rs = seed | 1;
    for (int run = 0; run < 20; run++) test_random(iterations * 5);
    printf("uheap_host: randomized runs pass (20 x %d steps)\n", iterations * 5);
    return 0;
}
