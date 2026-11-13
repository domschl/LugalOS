/* drivers/ttf_cache.c — PSRAM-backed TrueType font loader & 4-bit glyph cache.
 *
 * Allocates the font file buffer (for fonts <= 4 MB), rasterizer scratch,
 * 4-way set-associative glyph table, and 512 KB coverage arena via
 * palloc_pages_bulk() so internal SRAM is untouched.
 */

#include "lugalos_config.h"
#include "drivers/ttf_cache.h"
#include <string.h>

#if defined(CONFIG_PSRAM_BYTES) || defined(CONFIG_BOARD_VISIONFIVE2)

#include "fs/vfs.h"
#include "kernel/lock.h"
#include "kernel/palloc.h"

#define TTF_CACHE_SETS       256u
#define TTF_CACHE_WAYS       4u
#define TTF_CACHE_SLOTS      (TTF_CACHE_SETS * TTF_CACHE_WAYS)
#define TTF_ARENA_PAGES      128u /* 512 KB */
#define TTF_ARENA_BYTES      (TTF_ARENA_PAGES * 4096u)
#define TTF_MAX_MEM_FONT     (4u * 1024u * 1024u)

typedef struct {
    uint32_t            cp;
    uint16_t            cell_h;
    uint16_t            stamp;
    ttf_glyph_metrics_t m;
    uint32_t            aa_off; /* UINT32_MAX if empty bitmap */
} ttf_slot_t;

static ylock_t s_ttf_lock;

typedef struct {
    bool           loaded;
    bool           probed;
    ttf_font_t     font;
    uint8_t       *font_buf;
    uint32_t       font_pages;
    int            vfs_fd;

    ttf_scratch_t *scratch;
    uint32_t       scratch_pages;

    ttf_slot_t    *slots;
    uint32_t       slots_pages;

    uint8_t       *arena;
    uint32_t       arena_used;

    uint16_t       stamp;
    uint32_t       hits;
    uint32_t       misses;
    uint32_t       entries;
} ttf_cache_state_t;

/* guarded by: s_ttf_lock */
static ttf_cache_state_t s_tc;

static int vfs_read_cb(void *ctx, uint32_t off, void *dst, uint32_t len) {
    int fd = *(int *)ctx;
    uint8_t *out = (uint8_t *)dst;
    uint32_t done = 0;
    while (done < len) {
        int n = vfs_pread(fd, out + done, len - done, (uint64_t)off + done);
        if (n <= 0) return -1;
        done += (uint32_t)n;
    }
    return 0;
}

static bool ensure_workspace(void) {
    if (!s_tc.scratch) {
        uint32_t pages = (uint32_t)((sizeof(ttf_scratch_t) + 4095u) / 4096u);
        s_tc.scratch = (ttf_scratch_t *)palloc_pages_bulk(pages);
        if (!s_tc.scratch) return false;
        s_tc.scratch_pages = pages;
    }
    if (!s_tc.slots) {
        uint32_t pages = (uint32_t)((TTF_CACHE_SLOTS * sizeof(ttf_slot_t) + 4095u) / 4096u);
        s_tc.slots = (ttf_slot_t *)palloc_pages_bulk(pages);
        if (!s_tc.slots) return false;
        s_tc.slots_pages = pages;
        memset(s_tc.slots, 0, pages * 4096u);
    }
    if (!s_tc.arena) {
        s_tc.arena = (uint8_t *)palloc_pages_bulk(TTF_ARENA_PAGES);
        if (!s_tc.arena) return false;
        s_tc.arena_used = 0;
    }
    return true;
}

static void cache_clear(void) {
    if (s_tc.slots) {
        memset(s_tc.slots, 0, TTF_CACHE_SLOTS * sizeof(ttf_slot_t));
    }
    s_tc.arena_used = 0;
    s_tc.entries = 0;
    s_tc.stamp = 0;
}

void ttf_cache_unload(void) {
    ylock_acquire(&s_ttf_lock);
    s_tc.loaded = false;
    if (s_tc.font_buf) {
        palloc_free(s_tc.font_buf, s_tc.font_pages);
        s_tc.font_buf = NULL;
        s_tc.font_pages = 0;
    }
    if (s_tc.vfs_fd >= 0) {
        vfs_close(s_tc.vfs_fd);
        s_tc.vfs_fd = -1;
    }
    cache_clear();
    ylock_release(&s_ttf_lock);
}

bool ttf_cache_available(void) {
    ylock_acquire(&s_ttf_lock);
    bool ok = s_tc.loaded;
    ylock_release(&s_ttf_lock);
    return ok;
}

bool ttf_cache_load(const char *path) {
    if (!path || !*path) return false;
    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0 || st.is_dir || st.size < 64u) return false;

    ylock_acquire(&s_ttf_lock);
    if (!ensure_workspace()) {
        ylock_release(&s_ttf_lock);
        return false;
    }

    ttf_cache_unload();

    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        ylock_release(&s_ttf_lock);
        return false;
    }

    if (st.size <= TTF_MAX_MEM_FONT) {
        uint32_t pages = (st.size + 4095u) / 4096u;
        uint8_t *buf = (uint8_t *)palloc_pages_bulk(pages);
        if (!buf) {
            vfs_close(fd);
            ylock_release(&s_ttf_lock);
            return false;
        }
        uint32_t done = 0;
        while (done < st.size) {
            int n = vfs_pread(fd, buf + done, st.size - done, done);
            if (n <= 0) {
                palloc_free(buf, pages);
                vfs_close(fd);
                ylock_release(&s_ttf_lock);
                return false;
            }
            done += (uint32_t)n;
        }
        vfs_close(fd);

        if (!ttf_open_mem(&s_tc.font, buf, st.size)) {
            palloc_free(buf, pages);
            ylock_release(&s_ttf_lock);
            return false;
        }
        s_tc.font_buf = buf;
        s_tc.font_pages = pages;
    } else {
        s_tc.vfs_fd = fd;
        if (!ttf_open_io(&s_tc.font, vfs_read_cb, &s_tc.vfs_fd, st.size)) {
            vfs_close(fd);
            s_tc.vfs_fd = -1;
            ylock_release(&s_ttf_lock);
            return false;
        }
    }

    s_tc.loaded = true;
    s_tc.probed = true;
    s_tc.hits = 0;
    s_tc.misses = 0;
    ylock_release(&s_ttf_lock);
    return true;
}

bool ttf_cache_ensure_default(void) {
    ylock_acquire(&s_ttf_lock);
    if (s_tc.loaded) {
        ylock_release(&s_ttf_lock);
        return true;
    }
    if (s_tc.probed) {
        ylock_release(&s_ttf_lock);
        return false;
    }
    s_tc.probed = true;
    ylock_release(&s_ttf_lock);

    static const char *const k_paths[] = {
        "/nvme0/fonts/NotoSerif-Regular.ttf",
        "/sd0/fonts/NotoSerif-Regular.ttf",
    };
    for (size_t i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]); i++) {
        if (ttf_cache_load(k_paths[i])) return true;
    }
    return false;
}

void ttf_cache_reset_probe(void) {
    ylock_acquire(&s_ttf_lock);
    s_tc.probed = false;
    ylock_release(&s_ttf_lock);
}

static uint32_t hash_key(int cell_h, uint32_t cp) {
    uint32_t h = cp * 2654435761u ^ ((uint32_t)cell_h * 40503u);
    return h & (TTF_CACHE_SETS - 1u);
}

const ttf_glyph_metrics_t *ttf_cache_get(int cell_h, uint32_t codepoint,
                                         const uint8_t **out_aa) {
    if (out_aa) *out_aa = NULL;
    if (cell_h <= 0) return NULL;
    ylock_acquire(&s_ttf_lock);
    if (!s_tc.loaded || !s_tc.slots || !s_tc.arena) {
        ylock_release(&s_ttf_lock);
        return NULL;
    }

    uint32_t set = hash_key(cell_h, codepoint);
    ttf_slot_t *bucket = &s_tc.slots[set * TTF_CACHE_WAYS];
    s_tc.stamp++;

    int empty_idx = -1;
    int lru_idx = 0;
    uint16_t best_age = 0;

    for (int w = 0; w < (int)TTF_CACHE_WAYS; w++) {
        if (bucket[w].cell_h == (uint16_t)cell_h && bucket[w].cp == codepoint) {
            bucket[w].stamp = s_tc.stamp;
            s_tc.hits++;
            if (out_aa) {
                *out_aa = (bucket[w].aa_off == UINT32_MAX)
                          ? NULL
                          : (s_tc.arena + bucket[w].aa_off);
            }
            const ttf_glyph_metrics_t *res = &bucket[w].m;
            ylock_release(&s_ttf_lock);
            return res;
        }
        if (bucket[w].cell_h == 0 && empty_idx < 0) {
            empty_idx = w;
        } else {
            uint16_t age = (uint16_t)(s_tc.stamp - bucket[w].stamp);
            if (age >= best_age) {
                best_age = age;
                lru_idx = w;
            }
        }
    }

    s_tc.misses++;
    uint8_t tmp_aa[TTF_MAX_GLYPH_W * TTF_MAX_GLYPH_H / 2];
    ttf_glyph_metrics_t m;
    if (!ttf_rasterize_glyph(&s_tc.font, s_tc.scratch, cell_h, codepoint,
                             &m, tmp_aa, sizeof(tmp_aa))) {
        ylock_release(&s_ttf_lock);
        return NULL;
    }

    uint32_t bytes = (uint32_t)m.h * m.stride;
    uint32_t aligned = (bytes + 3u) & ~3u;
    if (aligned > 0 && s_tc.arena_used + aligned > TTF_ARENA_BYTES) {
        cache_clear();
        bucket = &s_tc.slots[set * TTF_CACHE_WAYS];
        empty_idx = 0;
    }

    int target = (empty_idx >= 0) ? empty_idx : lru_idx;
    if (bucket[target].cell_h == 0) s_tc.entries++;

    bucket[target].cp = codepoint;
    bucket[target].cell_h = (uint16_t)cell_h;
    bucket[target].stamp = s_tc.stamp;
    bucket[target].m = m;
    if (bytes > 0) {
        bucket[target].aa_off = s_tc.arena_used;
        memcpy(s_tc.arena + s_tc.arena_used, tmp_aa, bytes);
        s_tc.arena_used += aligned;
        if (out_aa) *out_aa = s_tc.arena + bucket[target].aa_off;
    } else {
        bucket[target].aa_off = UINT32_MAX;
        if (out_aa) *out_aa = NULL;
    }
    const ttf_glyph_metrics_t *res = &bucket[target].m;
    ylock_release(&s_ttf_lock);
    return res;
}

void ttf_cache_warm_ascii(int cell_h) {
    if (cell_h <= 0) return;
    for (uint32_t cp = 0x20u; cp <= 0x7Eu; cp++) {
        (void)ttf_cache_get(cell_h, cp, NULL);
    }
}

void ttf_cache_stats(uint32_t *out_hits, uint32_t *out_misses,
                     uint32_t *out_entries, uint32_t *out_arena_used,
                     uint32_t *out_arena_cap) {
    ylock_acquire(&s_ttf_lock);
    if (out_hits) *out_hits = s_tc.hits;
    if (out_misses) *out_misses = s_tc.misses;
    if (out_entries) *out_entries = s_tc.entries;
    if (out_arena_used) *out_arena_used = s_tc.arena_used;
    if (out_arena_cap) *out_arena_cap = s_tc.arena ? TTF_ARENA_BYTES : 0u;
    ylock_release(&s_ttf_lock);
}

#else /* !CONFIG_PSRAM_BYTES && !CONFIG_BOARD_VISIONFIVE2 */

bool ttf_cache_available(void) { return false; }
bool ttf_cache_load(const char *path) { (void)path; return false; }
void ttf_cache_unload(void) {}
bool ttf_cache_ensure_default(void) { return false; }
void ttf_cache_reset_probe(void) {}
const ttf_glyph_metrics_t *ttf_cache_get(int cell_h, uint32_t codepoint,
                                         const uint8_t **out_aa) {
    (void)cell_h; (void)codepoint;
    if (out_aa) *out_aa = NULL;
    return NULL;
}
void ttf_cache_warm_ascii(int cell_h) { (void)cell_h; }
void ttf_cache_stats(uint32_t *out_hits, uint32_t *out_misses,
                     uint32_t *out_entries, uint32_t *out_arena_used,
                     uint32_t *out_arena_cap) {
    if (out_hits) *out_hits = 0;
    if (out_misses) *out_misses = 0;
    if (out_entries) *out_entries = 0;
    if (out_arena_used) *out_arena_used = 0;
    if (out_arena_cap) *out_arena_cap = 0;
}

#endif
