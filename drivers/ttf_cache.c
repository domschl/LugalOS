/* drivers/ttf_cache.c — PSRAM-backed TrueType font loader, OpenType shaper,
 * and 4-bit glyph cache.
 *
 * Allocates font file buffers, OpenType tables, rasterizer scratch,
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

#define OT_TAG(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
     ((uint32_t)(uint8_t)(c) << 8)  | (uint32_t)(uint8_t)(d))

typedef struct {
    uint16_t            gid;
    uint8_t             font_id;
    uint8_t             pad;
    uint16_t            cell_h;
    uint16_t            stamp;
    ttf_glyph_metrics_t m;
    uint32_t            aa_off; /* UINT32_MAX if empty bitmap */
} ttf_slot_t;

typedef struct {
    bool       loaded;
    bool       probed;
    ttf_font_t font;
    ttf_ot_t   ot;
    uint8_t   *font_buf;
    uint32_t   font_pages;
    uint8_t   *ot_buf;
    uint32_t   ot_pages;
    int        vfs_fd;
} ttf_font_slot_t;

static ylock_t s_ttf_lock;

typedef struct {
    ttf_font_slot_t fonts[TTF_FONT_SLOTS];

    ttf_scratch_t  *scratch;
    uint32_t        scratch_pages;

    ttf_slot_t     *slots;
    uint32_t        slots_pages;

    uint8_t        *arena;
    uint32_t        arena_used;

    uint8_t         tmp_aa[TTF_MAX_GLYPH_W * TTF_MAX_GLYPH_H / 2];
    uint16_t        tmp_gids[TTF_MAX_SHAPED];
    int16_t         tmp_kern_fu[TTF_MAX_SHAPED];

    uint16_t        stamp;
    uint32_t        hits;
    uint32_t        misses;
    uint32_t        entries;
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

static void unload_slot_locked(uint8_t font_id) {
    if (font_id >= TTF_FONT_SLOTS) return;
    ttf_font_slot_t *fs = &s_tc.fonts[font_id];
    fs->loaded = false;
    if (fs->ot_buf) {
        palloc_free(fs->ot_buf, fs->ot_pages);
        fs->ot_buf = NULL;
        fs->ot_pages = 0;
    }
    memset(&fs->ot, 0, sizeof(fs->ot));
    if (fs->font_buf) {
        palloc_free(fs->font_buf, fs->font_pages);
        fs->font_buf = NULL;
        fs->font_pages = 0;
    }
    if (fs->vfs_fd > 0 || (fs->vfs_fd == 0 && fs->font.read_fn == vfs_read_cb)) {
        vfs_close(fs->vfs_fd);
    }
    fs->vfs_fd = -1;
    memset(&fs->font, 0, sizeof(fs->font));
    cache_clear();
}

void ttf_cache_unload(void) {
    ylock_acquire(&s_ttf_lock);
    for (uint8_t i = 0; i < TTF_FONT_SLOTS; i++) {
        unload_slot_locked(i);
    }
    ylock_release(&s_ttf_lock);
}

bool ttf_cache_available(void) {
    ylock_acquire(&s_ttf_lock);
    bool ok = s_tc.fonts[TTF_FONT_PRIMARY].loaded;
    ylock_release(&s_ttf_lock);
    return ok;
}

bool ttf_cache_load_slot(uint8_t font_id, const char *path, uint32_t script_tag) {
    if (font_id >= TTF_FONT_SLOTS || !path || !*path) return false;
    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0 || st.is_dir || st.size < 64u) return false;

    ylock_acquire(&s_ttf_lock);
    if (!ensure_workspace()) {
        ylock_release(&s_ttf_lock);
        return false;
    }

    unload_slot_locked(font_id);
    ttf_font_slot_t *fs = &s_tc.fonts[font_id];

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

        if (!ttf_open_mem(&fs->font, buf, st.size)) {
            palloc_free(buf, pages);
            ylock_release(&s_ttf_lock);
            return false;
        }
        fs->font_buf = buf;
        fs->font_pages = pages;
    } else {
        fs->vfs_fd = fd;
        if (!ttf_open_io(&fs->font, vfs_read_cb, &fs->vfs_fd, st.size)) {
            vfs_close(fd);
            fs->vfs_fd = -1;
            ylock_release(&s_ttf_lock);
            return false;
        }
    }

    uint32_t ot_bytes = ttf_ot_required_bytes(&fs->font);
    if (ot_bytes > 0) {
        uint32_t ot_pages = (ot_bytes + 4095u) / 4096u;
        uint8_t *obuf = (uint8_t *)palloc_pages_bulk(ot_pages);
        if (obuf) {
            if (ttf_ot_init(&fs->ot, &fs->font, script_tag, obuf, ot_pages * 4096u)) {
                fs->ot_buf = obuf;
                fs->ot_pages = ot_pages;
            } else {
                palloc_free(obuf, ot_pages);
            }
        }
    }

    fs->loaded = true;
    fs->probed = true;
    if (font_id == TTF_FONT_PRIMARY) {
        s_tc.hits = 0;
        s_tc.misses = 0;
    }
    ylock_release(&s_ttf_lock);
    return true;
}

bool ttf_cache_load(const char *path) {
    return ttf_cache_load_slot(TTF_FONT_PRIMARY, path, OT_TAG('l', 'a', 't', 'n'));
}

bool ttf_cache_ensure_default(void) {
    if (!vfs_volume_mounted("sd0") && !vfs_volume_mounted("nvme0")) return false;
    ylock_acquire(&s_ttf_lock);
    ttf_font_slot_t *fs = &s_tc.fonts[TTF_FONT_PRIMARY];
    if (fs->loaded) {
        ylock_release(&s_ttf_lock);
        return true;
    }
    if (fs->probed) {
        ylock_release(&s_ttf_lock);
        return false;
    }
    fs->probed = true;
    ylock_release(&s_ttf_lock);

    static const char *const k_paths[] = {
        "/nvme0/fonts/NotoSerif-Regular.ttf",
        "/sd0/fonts/NotoSerif-Regular.ttf",
    };
    for (size_t i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]); i++) {
        if (ttf_cache_load_slot(TTF_FONT_PRIMARY, k_paths[i], OT_TAG('l', 'a', 't', 'n'))) return true;
    }
    return false;
}

bool ttf_cache_ensure_tibetan(void) {
    if (!vfs_volume_mounted("sd0") && !vfs_volume_mounted("nvme0")) return false;
    ylock_acquire(&s_ttf_lock);
    ttf_font_slot_t *fs = &s_tc.fonts[TTF_FONT_TIBETAN];
    if (fs->loaded) {
        ylock_release(&s_ttf_lock);
        return true;
    }
    if (fs->probed) {
        ylock_release(&s_ttf_lock);
        return false;
    }
    fs->probed = true;
    ylock_release(&s_ttf_lock);

    static const char *const k_paths[] = {
        "/nvme0/fonts/Jomolhari-Regular.ttf",
        "/sd0/fonts/Jomolhari-Regular.ttf",
    };
    for (size_t i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]); i++) {
        if (ttf_cache_load_slot(TTF_FONT_TIBETAN, k_paths[i], OT_TAG('t', 'i', 'b', 't'))) return true;
    }
    return false;
}

bool ttf_cache_ensure_math(void) {
    if (!vfs_volume_mounted("sd0") && !vfs_volume_mounted("nvme0")) return false;
    ylock_acquire(&s_ttf_lock);
    ttf_font_slot_t *fs = &s_tc.fonts[TTF_FONT_MATH];
    if (fs->loaded) {
        ylock_release(&s_ttf_lock);
        return true;
    }
    if (fs->probed) {
        ylock_release(&s_ttf_lock);
        return false;
    }
    fs->probed = true;
    ylock_release(&s_ttf_lock);

    static const char *const k_paths[] = {
        "/nvme0/fonts/NotoSansMath-Regular.ttf",
        "/sd0/fonts/NotoSansMath-Regular.ttf",
    };
    for (size_t i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]); i++) {
        if (ttf_cache_load_slot(TTF_FONT_MATH, k_paths[i], OT_TAG('m', 'a', 't', 'h'))) return true;
    }
    return false;
}

void ttf_cache_reset_probe(void) {
    ylock_acquire(&s_ttf_lock);
    for (uint8_t i = 0; i < TTF_FONT_SLOTS; i++) {
        s_tc.fonts[i].probed = false;
    }
    ylock_release(&s_ttf_lock);
}

static uint32_t hash_key(uint8_t font_id, int cell_h, uint16_t gid) {
    uint32_t k = ((uint32_t)font_id << 16) | gid;
    uint32_t h = k * 2654435761u ^ ((uint32_t)cell_h * 40503u);
    return h & (TTF_CACHE_SETS - 1u);
}

const ttf_glyph_metrics_t *ttf_cache_get_gid(uint8_t font_id, int cell_h,
                                             uint16_t gid, const uint8_t **out_aa) {
    if (out_aa) *out_aa = NULL;
    if (font_id >= TTF_FONT_SLOTS || cell_h <= 0) return NULL;
    ylock_acquire(&s_ttf_lock);
    ttf_font_slot_t *fs = &s_tc.fonts[font_id];
    if (!fs->loaded || !s_tc.slots || !s_tc.arena) {
        ylock_release(&s_ttf_lock);
        return NULL;
    }

    uint32_t set = hash_key(font_id, cell_h, gid);
    ttf_slot_t *bucket = &s_tc.slots[set * TTF_CACHE_WAYS];
    s_tc.stamp++;

    int empty_idx = -1;
    int lru_idx = 0;
    uint16_t best_age = 0;

    for (int w = 0; w < (int)TTF_CACHE_WAYS; w++) {
        if (bucket[w].cell_h == (uint16_t)cell_h &&
            bucket[w].font_id == font_id &&
            bucket[w].gid == gid) {
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
    ttf_glyph_metrics_t m;
    if (!ttf_rasterize_glyph_id(&fs->font, s_tc.scratch, cell_h, gid,
                                &m, s_tc.tmp_aa, sizeof(s_tc.tmp_aa))) {
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

    bucket[target].gid = gid;
    bucket[target].font_id = font_id;
    bucket[target].pad = 0;
    bucket[target].cell_h = (uint16_t)cell_h;
    bucket[target].stamp = s_tc.stamp;
    bucket[target].m = m;
    if (bytes > 0) {
        bucket[target].aa_off = s_tc.arena_used;
        memcpy(s_tc.arena + s_tc.arena_used, s_tc.tmp_aa, bytes);
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

const ttf_glyph_metrics_t *ttf_cache_get(int cell_h, uint32_t codepoint,
                                         const uint8_t **out_aa) {
    uint8_t font_id = TTF_FONT_PRIMARY;
    if (codepoint >= 0x0F00u && codepoint <= 0x0FFFu && ttf_cache_ensure_tibetan()) {
        font_id = TTF_FONT_TIBETAN;
    }
    ylock_acquire(&s_ttf_lock);
    ttf_font_slot_t *fs = &s_tc.fonts[font_id];
    uint16_t gid = fs->loaded ? ttf_glyph_index(&fs->font, codepoint) : 0;
    bool loaded = fs->loaded;
    ylock_release(&s_ttf_lock);
    if (!loaded) {
        if (out_aa) *out_aa = NULL;
        return NULL;
    }
    return ttf_cache_get_gid(font_id, cell_h, gid, out_aa);
}

const ttf_glyph_metrics_t *ttf_cache_get_exact(int cell_h, uint32_t codepoint,
                                               const uint8_t **out_aa) {
    uint8_t font_id = TTF_FONT_PRIMARY;
    if (codepoint >= 0x0F00u && codepoint <= 0x0FFFu && ttf_cache_ensure_tibetan()) {
        font_id = TTF_FONT_TIBETAN;
    }
    ylock_acquire(&s_ttf_lock);
    ttf_font_slot_t *fs = &s_tc.fonts[font_id];
    uint16_t gid = fs->loaded ? ttf_glyph_index(&fs->font, codepoint) : 0;
    bool loaded = fs->loaded;
    ylock_release(&s_ttf_lock);
    if ((!loaded || gid == 0) && font_id == TTF_FONT_PRIMARY && ttf_cache_ensure_math()) {
        ylock_acquire(&s_ttf_lock);
        ttf_font_slot_t *mfs = &s_tc.fonts[TTF_FONT_MATH];
        uint16_t mgid = mfs->loaded ? ttf_glyph_index(&mfs->font, codepoint) : 0;
        ylock_release(&s_ttf_lock);
        if (mgid != 0) {
            return ttf_cache_get_gid(TTF_FONT_MATH, cell_h, mgid, out_aa);
        }
    }
    if (!loaded || gid == 0) {
        if (out_aa) *out_aa = NULL;
        return NULL;
    }
    return ttf_cache_get_gid(font_id, cell_h, gid, out_aa);
}

int ttf_cache_baseline_px(int cell_h) {
    ylock_acquire(&s_ttf_lock);
    ttf_font_slot_t *fs = &s_tc.fonts[TTF_FONT_PRIMARY];
    int bl = fs->loaded ? ttf_baseline_px(&fs->font, cell_h) : (cell_h * 3) / 4;
    ylock_release(&s_ttf_lock);
    return bl;
}

int ttf_cache_shape_run(const uint32_t *cps, int count, int cell_h,
                        uint8_t *out_font_id, ttf_shaped_glyph_t *out, int max_out) {
    if (!cps || count <= 0 || !out || max_out <= 0 || cell_h <= 0) return 0;

    uint8_t font_id = TTF_FONT_PRIMARY;
    if (cps[0] >= 0x0F00u && cps[0] <= 0x0FFFu && ttf_cache_ensure_tibetan()) {
        font_id = TTF_FONT_TIBETAN;
    }
    if (out_font_id) *out_font_id = font_id;

    int lim = max_out < TTF_MAX_SHAPED ? max_out : TTF_MAX_SHAPED;

    ylock_acquire(&s_ttf_lock);
    ttf_font_slot_t *fs = &s_tc.fonts[font_id];
    if (!fs->loaded) {
        ylock_release(&s_ttf_lock);
        return 0;
    }
    int n = ttf_ot_shape(&fs->font, &fs->ot, cps, count, s_tc.tmp_gids, s_tc.tmp_kern_fu, lim);
    for (int i = 0; i < n; i++) {
        out[i].gid = s_tc.tmp_gids[i];
        out[i].kern_px = (int16_t)ttf_ot_scale_fu(&fs->font, cell_h, s_tc.tmp_kern_fu[i]);
    }
    ylock_release(&s_ttf_lock);
    return n;
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
bool ttf_cache_load_slot(uint8_t font_id, const char *path, uint32_t script_tag) {
    (void)font_id; (void)path; (void)script_tag; return false;
}
void ttf_cache_unload(void) {}
bool ttf_cache_ensure_default(void) { return false; }
bool ttf_cache_ensure_tibetan(void) { return false; }
bool ttf_cache_ensure_math(void) { return false; }
void ttf_cache_reset_probe(void) {}
const ttf_glyph_metrics_t *ttf_cache_get(int cell_h, uint32_t codepoint,
                                         const uint8_t **out_aa) {
    (void)cell_h; (void)codepoint;
    if (out_aa) *out_aa = NULL;
    return NULL;
}
const ttf_glyph_metrics_t *ttf_cache_get_exact(int cell_h, uint32_t codepoint,
                                               const uint8_t **out_aa) {
    (void)cell_h; (void)codepoint;
    if (out_aa) *out_aa = NULL;
    return NULL;
}
int ttf_cache_baseline_px(int cell_h) { return (cell_h * 3) / 4; }
const ttf_glyph_metrics_t *ttf_cache_get_gid(uint8_t font_id, int cell_h,
                                             uint16_t gid, const uint8_t **out_aa) {
    (void)font_id; (void)cell_h; (void)gid;
    if (out_aa) *out_aa = NULL;
    return NULL;
}
int ttf_cache_shape_run(const uint32_t *cps, int count, int cell_h,
                        uint8_t *out_font_id, ttf_shaped_glyph_t *out, int max_out) {
    (void)cps; (void)count; (void)cell_h; (void)out_font_id; (void)out; (void)max_out;
    return 0;
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
