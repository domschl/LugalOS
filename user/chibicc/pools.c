#include "chibicc.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include <string.h>

/* chibicc's working memory, taken from the heap only while it is compiling
 * (C6, plan/phase6_memory_and_processes.md).
 *
 * It used to be eleven static arrays across four files -- token and node and
 * type pools, the macro table, the preprocessor buffer, the source and output
 * buffers -- adding up to about 108 KB. On a board with 512 KB of SRAM that is
 * a fifth of the machine reserved for a compiler that is idle almost always.
 * It was the single largest subsystem in the image.
 *
 * ## Why this rather than making cc a process
 *
 * The plan's C6 proposed extracting chibicc into a U-mode executable, which
 * would have freed the same memory and confined it as well. Two things decided
 * against it for now, and both are worth recording because they are not
 * obvious:
 *
 *   - **Peak memory is worse as a process.** A user image must be granted in
 *     NAPOT regions, so 108 KB of pools becomes a 128 KB power-of-two region:
 *     20 KB of padding, and it must be placed at a 128 KB-aligned address. An
 *     arena needs neither -- it is pages, at any address.
 *
 *   - **It calls vfs_read()/vfs_write() directly**, as kernel code. In U-mode
 *     those become SYS_READ_FILE/SYS_WRITE_FILE, whose kernel-side buffer is
 *     512 bytes, and chibicc reads 4 KB sources and writes 4 KB ELFs. A
 *     process version needs a chunked or descriptor-based file ABI first.
 *
 * The isolation argument is real -- a compiler consuming arbitrary input is
 * exactly what one would want confined -- but nothing here forecloses it: a
 * `cc.elf` on the search path can be added later, and making the pools dynamic
 * is a prerequisite for that rather than a detour from it. On this hardware
 * memory is the binding constraint, so memory is what this optimises.
 *
 * ## One arena, not eleven allocations
 *
 * A bump allocator over a single page run. Eleven separate palloc calls would
 * each round up to a page boundary and waste roughly 6 KB between them, and
 * would give eleven failure points to unwind instead of one. Nothing here ever
 * frees an individual pool -- the whole arena goes back when the compile ends
 * -- so a bump pointer is the entire allocator this needs.
 */

/* The size is asked of the modules rather than written down here.
 *
 * A constant would have to track a set of #if'd limits in three other files:
 * MAX_NODES is 256 on RP2350 and 2048 elsewhere, and the rest differ likewise,
 * so one number is wrong on at least one target by a factor of eight. It was,
 * on the first attempt -- 112 KB against the ~300 KB the QEMU builds want --
 * and the failure was a clean diagnostic only because the bump allocator
 * checks. Summing what each module asks for cannot drift. */
static uint8_t *g_arena;
static uint32_t g_pages;
static uint32_t g_used;
static uint32_t g_cap;
static bool     g_overflowed;

void *chibicc_pool_alloc(uint32_t bytes) {
    if (!g_arena) return NULL;

    /* Pointer-aligned: the arena hands out Token and Node arrays, not just
     * byte buffers. */
    uint32_t align = (uint32_t)sizeof(void *);
    g_used = (g_used + align - 1) & ~(align - 1);

    if (g_used + bytes > g_cap) {
        if (!g_overflowed) {
            printk("[chibicc Error] Pool arena of %u KB is too small; a pool "
                   "asked for %u more bytes than it declared\n",
                   g_cap / 1024, bytes);
            g_overflowed = true;
        }
        return NULL;
    }

    void *p = g_arena + g_used;
    g_used += bytes;
    return p;
}

bool chibicc_pools_acquire(void) {
    if (g_arena) return true; /* already held: a nested compile reuses it */

    /* Each pool is pointer-aligned on the way out, so allow a machine word of
     * slack per allocation rather than counting them exactly. */
    uint32_t want = tokenize_pools_bytes() + parse_pools_bytes() +
                    preprocess_pools_bytes() + main_pools_bytes() + 128u;
    g_pages = (want + (uint32_t)PAGE_SIZE - 1) / (uint32_t)PAGE_SIZE;

    g_arena = (uint8_t *)palloc_pages(g_pages);
    if (!g_arena) {
        printk("[chibicc Error] No memory for a %u KB compiler arena; cc needs "
               "it only while compiling, so try again with less running\n",
               g_pages * (uint32_t)PAGE_SIZE / 1024);
        return false;
    }
    g_cap = g_pages * (uint32_t)PAGE_SIZE;
    g_used = 0;
    g_overflowed = false;

    /* Each module claims its own pools. Order does not matter -- the arena is
     * a bump pointer -- but every one of these must succeed before any of them
     * is used, which is why they are all here rather than lazily on first
     * touch. */
    if (!tokenize_pools_init() || !parse_pools_init() ||
        !preprocess_pools_init() || !main_pools_init()) {
        chibicc_pools_release();
        return false;
    }
    return true;
}

void chibicc_pools_release(void) {
    if (!g_arena) return;
    palloc_free(g_arena, g_pages);
    g_arena = NULL;
    g_pages = 0;
    g_used = 0;
    g_cap = 0;
    /* The modules' pointers are now dangling, so anything that used them after
     * this would be a use-after-free rather than a quiet wrong answer. They are
     * cleared by the next acquire, and nothing outside a compile touches them. */
    tokenize_pools_clear();
    parse_pools_clear();
    preprocess_pools_clear();
    main_pools_clear();
}
