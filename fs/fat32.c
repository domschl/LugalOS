#include "fs/fat32.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include <string.h>

/* A 512-byte sector buffer declared as uint8_t[512] and then cast to
 * uint32_t* or fat32_dir_entry_t* for in-place field access is a strict
 * alignment violation per the C standard: uint8_t only guarantees 1-byte
 * alignment, and the compiler is free to place such an array anywhere.
 * UBSan's alignment check (enabled via -fsanitize=undefined) has never
 * fired against this pattern across the full test suite, which means it
 * happens not to be a *live* bug for this compiler/target's stack layout
 * today -- but that's implementation behavior the standard doesn't
 * guarantee, not something this code can rely on (see B13/X.4 in
 * plan/completed/2026-08-07_review_and_remediation.md). This union gives a sector
 * buffer every view it's used as up front, so the compiler guarantees
 * correct alignment for all of them and no cast is needed at any access
 * site -- used only where a buffer is actually read through one of the
 * typed views; buffers only ever touched via memcpy() (already
 * alignment-safe) are left as plain uint8_t[512]. */
typedef union {
    uint8_t raw[512];
    uint32_t words[128];
    fat32_dir_entry_t entries[16];
    fat32_bpb_t bpb;
} fat32_sector_t;

/* --- Names -----------------------------------------------------------------
 *
 * A name is UTF-8 in the VFS and up to FAT32_LFN_MAX UTF-16 units on disk, in
 * a set of long-name slots ahead of its 8.3 entry (VFAT). Matching ignores
 * case for ASCII and Latin-1 letters. A name that is an 8.3 name exactly, each
 * part in one case, is stored as that 8.3 entry alone with its case in the
 * entry's NT flags (as Windows and Linux store "readme.txt"); any other name
 * gets a long-name set and a unique "~N" alias. */

#define LFN_UNITS      13u          /* UTF-16 units per long-name slot */
#define LFN_MAX_SLOTS  20u          /* ceil(FAT32_LFN_MAX / LFN_UNITS) */
#define NT_LOWER_BASE  0x08u
#define NT_LOWER_EXT   0x10u

/* Where a long-name slot keeps its 13 units, two bytes each, little-endian. */
static const uint8_t k_lfn_off[LFN_UNITS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

/* UTF-8 read as UTF-16 units, one at a time. */
typedef struct {
    const uint8_t *p, *end;
    uint16_t low;              /* a surrogate pair's second half, still to come */
} u16_iter_t;

static void u16_init(u16_iter_t *it, const char *s, size_t len) {
    it->p = (const uint8_t *)s;
    it->end = it->p + len;
    it->low = 0;
}

/* The next unit; -1 at the end, -2 for bytes that are not UTF-8 (overlong, a
 * surrogate, past U+10FFFF, or cut short). */
static int32_t u16_next(u16_iter_t *it) {
    if (it->low) {
        int32_t r = it->low;
        it->low = 0;
        return r;
    }
    if (it->p >= it->end) return -1;
    uint32_t c = *it->p++;
    if (c < 0x80) return (int32_t)c;
    int more;
    uint32_t min;
    if ((c & 0xE0) == 0xC0)      { more = 1; c &= 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { more = 2; c &= 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { more = 3; c &= 0x07; min = 0x10000; }
    else return -2;
    if (it->end - it->p < more) return -2;
    for (int i = 0; i < more; i++) {
        uint8_t b = *it->p++;
        if ((b & 0xC0) != 0x80) return -2;
        c = (c << 6) | (b & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return -2;
    if (c >= 0x10000) {
        c -= 0x10000;
        it->low = (uint16_t)(0xDC00 | (c & 0x3FF));
        return (int32_t)(0xD800 | (c >> 10));
    }
    return (int32_t)c;
}

/* Appends code point `c` as UTF-8 at out[*n]; false, with nothing written,
 * when it would not leave room for the NUL within `max`. */
static bool utf8_put(char *out, uint32_t max, uint32_t *n, uint32_t c) {
    uint8_t b[4];
    uint32_t k;
    if (c < 0x80) { b[0] = (uint8_t)c; k = 1; }
    else if (c < 0x800) { b[0] = (uint8_t)(0xC0 | (c >> 6)); b[1] = (uint8_t)(0x80 | (c & 0x3F)); k = 2; }
    else if (c < 0x10000) {
        b[0] = (uint8_t)(0xE0 | (c >> 12)); b[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
        b[2] = (uint8_t)(0x80 | (c & 0x3F)); k = 3;
    } else {
        b[0] = (uint8_t)(0xF0 | (c >> 18)); b[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
        b[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); b[3] = (uint8_t)(0x80 | (c & 0x3F)); k = 4;
    }
    if (*n + k >= max) return false;
    memcpy(out + *n, b, k);
    *n += k;
    return true;
}

/* ASCII and Latin-1 letters to upper case, for matching. */
static uint16_t fold(uint32_t u) {
    if (u >= 'a' && u <= 'z') return (uint16_t)(u - 32);
    if (u >= 0xE0 && u <= 0xFE && u != 0xF7) return (uint16_t)(u - 32);
    return (uint16_t)u;
}

/* A character an 8.3 name may hold. */
static bool is_83_char(uint32_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
    return c > 0 && c < 0x80 && strchr("!#$%&'()-@^_`{}~", (int)c) != NULL;
}

/* The name's length in UTF-16 units, or -1 when it is not UTF-8. */
static int32_t name_count_units(const char *name, size_t len) {
    u16_iter_t it;
    u16_init(&it, name, len);
    int32_t n = 0, u;
    while ((u = u16_next(&it)) >= 0) n++;
    return u == -1 ? n : -1;
}

/* The name's length in units if a file may be created with it, else -1:
 * UTF-8, 1..FAT32_LFN_MAX units, none of the characters FAT forbids, not "."
 * or "..", and no trailing dot or space (Windows drops those, so two
 * different names would be one file there). */
static int32_t name_units(const char *name, size_t len) {
    if (len == 0) return -1;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return -1;
    if (name[len - 1] == '.' || name[len - 1] == ' ') return -1;
    u16_iter_t it;
    u16_init(&it, name, len);
    int32_t n = 0, u;
    while ((u = u16_next(&it)) >= 0) {
        if (u < 0x20 || u == 0x7F || (u < 0x80 && strchr("\"*/:<>?\\|", (int)u))) return -1;
        if (++n > FAT32_LFN_MAX) return -1;
    }
    return u == -1 ? n : -1;
}

/* An 8.3 entry's name as characters, lower case where its NT flags say so;
 * bytes 0x80..0xFF stand for Latin-1. Returns how many (at most 12). */
static uint32_t short_chars(const fat32_dir_entry_t *e, uint8_t out[12]) {
    uint32_t n = 0;
    for (int i = 0; i < 8 && e->name[i] != ' '; i++) {
        uint8_t c = (uint8_t)e->name[i];
        if (i == 0 && c == 0x05) c = 0xE5;          /* 0x05 spells a leading 0xE5 */
        if ((e->nt_res & NT_LOWER_BASE) && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        out[n++] = c;
    }
    if (e->name[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e->name[i] != ' '; i++) {
            uint8_t c = (uint8_t)e->name[i];
            if ((e->nt_res & NT_LOWER_EXT) && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
            out[n++] = c;
        }
    }
    return n;
}

/* An 8.3 entry's name as a UTF-8 string ("readme.txt", "LUGALO~1.MD"). */
static void short_display(const fat32_dir_entry_t *e, char *out, uint32_t max) {
    if (!out || max == 0) return;
    uint8_t c[12];
    uint32_t k = short_chars(e, c), n = 0;
    for (uint32_t i = 0; i < k; i++) {
        if (!utf8_put(out, max, &n, c[i])) break;
    }
    out[n] = '\0';
}

static bool short_matches(const fat32_dir_entry_t *e, const char *name, size_t len) {
    uint8_t c[12];
    uint32_t k = short_chars(e, c);
    u16_iter_t it;
    u16_init(&it, name, len);
    for (uint32_t i = 0; i < k; i++) {
        int32_t u = u16_next(&it);
        if (u < 0 || fold((uint32_t)u) != fold(c[i])) return false;
    }
    return u16_next(&it) == -1;
}

/* The checksum every long-name slot carries of the 8.3 name it belongs to. */
static uint8_t lfn_checksum(const char name[11]) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1u) << 7) + (s >> 1) + (uint8_t)name[i]);
    return s;
}

/* `name` as an 8.3 entry name when it is one exactly; `nt` gets the case
 * flags. "readme.txt" and "README.TXT" fit; "ReadMe.txt", "notes.lisp" and
 * "a b.txt" do not. */
static bool name_fits_83(const char *name, size_t len, char out[11], uint8_t *nt) {
    if (len == 0 || len > 12 || name[0] == '.') return false;
    size_t dot = len;
    for (size_t i = 0; i < len; i++) {
        if (name[i] != '.') continue;
        if (dot != len) return false;
        dot = i;
    }
    size_t blen = dot, elen = dot < len ? len - dot - 1 : 0;
    if (blen > 8 || elen > 3 || (dot < len && elen == 0)) return false;
    bool lower[2] = { false, false }, upper[2] = { false, false };
    memset(out, ' ', 11);
    for (size_t i = 0; i < len; i++) {
        if (i == dot) continue;
        uint8_t c = (uint8_t)name[i];
        if (!is_83_char(c)) return false;
        int part = i > dot ? 1 : 0;
        if (c >= 'a' && c <= 'z') { lower[part] = true; c = (uint8_t)(c - 32); }
        else if (c >= 'A' && c <= 'Z') upper[part] = true;
        out[part ? 8 + (i - dot - 1) : i] = (char)c;
    }
    if ((lower[0] && upper[0]) || (lower[1] && upper[1])) return false;
    *nt = (uint8_t)((lower[0] ? NT_LOWER_BASE : 0u) | (lower[1] ? NT_LOWER_EXT : 0u));
    return true;
}

/* What an alias is made from: upper case, spaces and leading dots dropped,
 * the extension after the last dot, and '_' for anything 8.3 cannot hold. */
static void alias_basis(const char *name, size_t len, char base[8], uint32_t *blen,
                        char ext[3], uint32_t *elen) {
    size_t start = 0;
    while (start < len && (name[start] == '.' || name[start] == ' ')) start++;
    size_t dot = len;
    for (size_t i = start; i < len; i++) {
        if (name[i] == '.') dot = i;
    }
    *blen = 0;
    *elen = 0;
    u16_iter_t it;
    u16_init(&it, name + start, len - start);
    for (;;) {
        size_t pos = start + (size_t)(it.p - (const uint8_t *)(name + start));
        bool second_half = it.low != 0;
        int32_t u = u16_next(&it);
        if (u < 0) break;
        if (second_half || u == ' ' || pos == dot) continue;
        bool in_ext = dot < len && pos > dot;
        if (!in_ext && u == '.') continue;
        char c = is_83_char((uint32_t)u) ? (char)fold((uint32_t)u) : '_';
        if (in_ext) {
            if (*elen < 3) ext[(*elen)++] = c;
        } else if (*blen < 8) {
            base[(*blen)++] = c;
        }
    }
    if (*blen == 0) base[(*blen)++] = '_';
}

#define ALIAS_PLAIN   4u
#define ALIAS_HASHED  9u
#define ALIAS_MAX     (ALIAS_PLAIN + ALIAS_HASHED)
#define ALIAS_ROUNDS  8u

/* Alias candidates for round `round`: "NOTES~1".."NOTES~4" in round 0, then
 * nine "NOxxxx~N" with xxxx a hash of the whole long name, another hash each
 * round. That is Windows' scheme: names sharing their first six letters by
 * the thousand (book chapters) still find a free alias in one directory scan.
 * Returns how many candidates it wrote. */
static uint32_t alias_candidates(const char *name, size_t len, uint32_t round, char cand[ALIAS_MAX][11]) {
    char base[8], ext[3];
    uint32_t blen, elen;
    alias_basis(name, len, base, &blen, ext, &elen);
    uint32_t h = 2166136261u ^ (round * 0x9E3779B9u);
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)name[i];
        h *= 16777619u;
    }
    uint16_t h16 = (uint16_t)(h ^ (h >> 16));
    static const char hex[] = "0123456789ABCDEF";
    uint32_t n = 0;
    for (uint32_t k = 0; k < ALIAS_MAX; k++) {
        bool plain = k < ALIAS_PLAIN;
        if (plain && round > 0) continue;
        char *c = cand[n++];
        memset(c, ' ', 11);
        memcpy(c + 8, ext, elen);
        uint32_t p;
        if (plain) {
            p = blen < 6 ? blen : 6;
            memcpy(c, base, p);
            c[p++] = '~';
            c[p++] = (char)('1' + k);
        } else {
            p = blen < 2 ? blen : 2;
            memcpy(c, base, p);
            for (int s = 12; s >= 0; s -= 4) c[p++] = hex[(h16 >> s) & 15u];
            c[p++] = '~';
            c[p++] = (char)('1' + (k - ALIAS_PLAIN));
        }
    }
    return n;
}

static uint32_t cluster_to_lba(fat32_fs_t *fs, uint32_t cluster) {
    if (!fs) return 0;
    return fs->data_start_sector + (cluster - 2) * fs->bpb.sec_per_clus;
}

/* FSInfo (FAT32 spec, "FAT32 FSInfo Sector Structure"): three signatures
 * and two hints, the free cluster count and where to look for the next free
 * cluster. Both are hints by the spec's own words, so a value that is out of
 * range is treated as "unknown", never trusted. */
#define FSINFO_LEAD_SIG   0x41615252u
#define FSINFO_STRUC_SIG  0x61417272u
#define FSINFO_TRAIL_SIG  0xAA550000u
#define FSINFO_W_LEAD     0
#define FSINFO_W_STRUC    (484 / 4)
#define FSINFO_W_FREE     (488 / 4)
#define FSINFO_W_NXT      (492 / 4)
#define FSINFO_W_TRAIL    (508 / 4)

static void fsinfo_write(fat32_fs_t *fs, uint32_t free_count) {
    if (!fs->fsinfo_lba || !fs->dev->write_blocks) return;
    fat32_sector_t sec;
    if (fs->dev->read_blocks(fs->dev, sec.raw, fs->fsinfo_lba, 1) != 0) return;
    if (sec.words[FSINFO_W_LEAD] != FSINFO_LEAD_SIG || sec.words[FSINFO_W_STRUC] != FSINFO_STRUC_SIG) return;
    sec.words[FSINFO_W_FREE] = free_count;
    sec.words[FSINFO_W_NXT] = fs->next_free;
    fs->dev->write_blocks(fs->dev, sec.raw, fs->fsinfo_lba, 1);
}

/* Called before every FAT change. The first change after mount (or after a
 * sync) withdraws the on-disk count, one sector write, so that FSInfo is
 * never left claiming a number the FAT no longer matches. */
static void fat_will_change(fat32_fs_t *fs) {
    fs->modified = true;
    if (fs->fsinfo_valid) {
        fsinfo_write(fs, FAT32_FREE_UNKNOWN);
        fs->fsinfo_valid = false;
    }
}

/* The FAT sector holding `cluster`'s entry, through the cache when the
 * volume has one. Returns NULL on a read error (with the cache emptied, so
 * a failed read is never served as data later). `tmp` is the caller's
 * buffer for the uncached case. */
static const uint32_t *fat_sector_words(fat32_fs_t *fs, uint32_t fat_sector, fat32_sector_t *tmp) {
    if (fs->fat_cache) {
        if (fs->fat_cache_lba != fat_sector) {
            fs->fat_cache_lba = 0;
            if (fs->dev->read_blocks(fs->dev, fs->fat_cache, fat_sector, 1) != 0) return NULL;
            fs->fat_cache_lba = fat_sector;
        }
        return fs->fat_cache;
    }
    if (fs->dev->read_blocks(fs->dev, tmp->raw, fat_sector, 1) != 0) return NULL;
    return tmp->words;
}

/* Reads the FAT entry for `cluster` into *out; false if its sector cannot be
 * read. */
static bool fat_read_entry(fat32_fs_t *fs, uint32_t cluster, uint32_t *out) {
    if (!fs || !fs->dev || !fs->dev->read_blocks) return false;
    uint32_t fat_sector = fs->fat_start_sector + (cluster * 4) / 512;
    uint32_t fat_offset = (cluster * 4) % 512;
    fat32_sector_t tmp;
    const uint32_t *w = fat_sector_words(fs, fat_sector, &tmp);
    if (!w) return false;
    *out = w[fat_offset / 4] & 0x0FFFFFFF;
    return true;
}

/* Read a 32-bit FAT entry for the given cluster */
static uint32_t fat_get_entry(fat32_fs_t *fs, uint32_t cluster) {
    uint32_t v;
    /* An unreadable FAT sector ends the chain: the old code returned
     * whatever the stack buffer held, which could be followed anywhere. */
    return fat_read_entry(fs, cluster, &v) ? v : 0x0FFFFFFF;
}

/* Write a 32-bit FAT entry for the given cluster (both FAT1 and FAT2) */
static void fat_set_entry(fat32_fs_t *fs, uint32_t cluster, uint32_t value) {
    if (!fs || !fs->dev || !fs->dev->write_blocks) return;
    uint32_t fat_sector = fs->fat_start_sector + (cluster * 4) / 512;
    uint32_t fat_offset = (cluster * 4) % 512;
    fat_will_change(fs);
    fat32_sector_t fat_sec;
    const uint32_t *w = fat_sector_words(fs, fat_sector, &fat_sec);
    if (!w) return;
    if (w != fat_sec.words) memcpy(fat_sec.words, w, 512);
    /* The top four bits of an entry are reserved and must be preserved. */
    fat_sec.words[fat_offset / 4] = (fat_sec.words[fat_offset / 4] & 0xF0000000u) | (value & 0x0FFFFFFFu);
    if (fs->fat_cache) memcpy(fs->fat_cache, fat_sec.words, 512);   /* write-through */
    fs->dev->write_blocks(fs->dev, fat_sec.raw, fat_sector, 1); // FAT1
    if (fs->bpb.num_fats > 1) {
        /* The second FAT copy starts fat_sz32 sectors after the first, not
         * a hardcoded +8 -- that only happened to be correct for volumes
         * this codebase's own fat32_format() created (which used an
         * 8-sector FAT until 38.6). Any normally-sized FAT32 volume (a real SD card
         * formatted by a PC) has a much larger FAT, and the hardcoded
         * offset silently wrote cluster-chain updates into whatever
         * unrelated sector was 8 sectors after FAT1 instead of into FAT2
         * (see B9 in plan/completed/2026-08-07_review_and_remediation.md). */
        fs->dev->write_blocks(fs->dev, fat_sec.raw, fat_sector + fs->bpb.fat_sz32, 1); // FAT2
    }
}

/* Frees an entire cluster chain by zeroing each FAT entry from
 * first_cluster onward. Tolerates being pointed at an already-partially- or
 * fully-freed chain (stops as soon as it reads a 0 entry) and never touches
 * the reserved cluster 0/1 FAT entries, so it's safe to call unconditionally
 * -- e.g. fat32_rmdir() frees a directory's own chain and then calls
 * fat32_remove_entry_nolock(), which (after the B8 fix below) also frees
 * whatever's left of the same chain. Also guards against a corrupt
 * self-referential chain looping forever. */
static void fat32_free_chain(fat32_fs_t *fs, uint32_t first_cluster) {
    uint32_t clus = first_cluster;
    fs->gen++;   /* every cursor on this volume may now point into freed space */
    while (clus >= 2 && clus < fs->total_clusters + 2) {
        uint32_t next = fat_get_entry(fs, clus);
        if (next == 0) break;   /* already free: the rest is not ours to count */
        fat_set_entry(fs, clus, 0x00000000);
        if (fs->free_count != FAT32_FREE_UNKNOWN) fs->free_count++;
        if (clus < fs->next_free) fs->next_free = clus;
        if (next == clus) break;
        clus = next;
    }
}

/* Dynamically allocate a free cluster from the FAT table.
 *
 * Searches from the next_free hint and wraps, rather than from cluster 2
 * every time: the old scan cost one FAT lookup per cluster already in use,
 * per allocation, which on a well-used card made every appended cluster
 * slower than the last. Clusters are numbered 2 .. total_clusters + 1, and
 * the bound matters (B9 in plan/completed/2026-08-07_review_and_remediation.md):
 * tot_sec32 counts sectors, not clusters, and scanning to it reached past
 * the data region. */
static uint32_t fat_alloc_cluster(fat32_fs_t *fs) {
    if (!fs || fs->total_clusters == 0) return 0;
    /* A free count of 0 is not trusted to refuse: it may come from an FSInfo
     * another writer let go stale (every card LugalOS wrote before 38.0), and
     * a stale-low count would report a volume full that is not. The scan
     * below is what decides; on a really full volume it costs one FAT pass. */
    uint32_t end = fs->total_clusters + 2;   /* exclusive */
    uint32_t start = (fs->next_free >= 2 && fs->next_free < end) ? fs->next_free : 2;
    uint32_t c = start;
    fs->alloc_io_error = false;
    do {
        /* An unreadable FAT ends the search; it does not mean "in use, try
         * the next one". A failed read empties the FAT cache, so with the
         * card pulled every cluster of the volume cost its own failed SD
         * read: the shell's history mkdir after the pull spun for the best
         * part of an hour on the LCD-7. */
        uint32_t v;
        if (!fat_read_entry(fs, c, &v)) {
            fs->alloc_io_error = true;
            return 0;
        }
        if (v == 0x00000000) {
            fat_set_entry(fs, c, 0x0FFFFFFF);
            if (fs->free_count == 0) fs->free_count = FAT32_FREE_UNKNOWN;   /* it was wrong */
            else if (fs->free_count != FAT32_FREE_UNKNOWN) fs->free_count--;
            fs->next_free = (c + 1 < end) ? c + 1 : 2;
            return c;
        }
        if (++c >= end) c = 2;
    } while (c != start);
    fs->free_count = 0;
    return 0;
}

/* Callback invoked once per 512-byte directory-entry sector while scanning
 * a directory's full cluster chain (see fat32_scan_dir below). `entries`
 * points at `count` (always 16) fat32_dir_entry_t records; `sector_lba` is
 * where they came from, so a callback that modifies an entry can write the
 * sector straight back with fs->dev->write_blocks(). Return true to stop
 * scanning (found what the caller wanted, hit the 0x00 end-of-directory
 * marker, or any other reason to end early); false to keep scanning. */
typedef bool (*fat32_dir_scan_fn)(fat32_fs_t *fs, uint32_t sector_lba,
                                   fat32_dir_entry_t *entries, int count, void *ctx);

/* Walks every sector of every cluster in the chain starting at start_clus,
 * calling `fn` once per sector, until `fn` returns true or the chain ends.
 * This is the one place that accounts for sec_per_clus sectors per
 * cluster -- every directory scan in this file used to read only a
 * cluster's first sector (16 entries), silently hiding any entries stored
 * in later sectors of a cluster. That went unnoticed because this
 * codebase's own fat32_format() used sec_per_clus = 1 (still does below 32 MB), but any
 * normally PC-formatted FAT32 card typically uses 8-64 sectors per cluster
 * (see B9 in plan/completed/2026-08-07_review_and_remediation.md). Two of the
 * write-path scans (fat32_write_file's and fat32_mkdir's old free-slot
 * search) didn't even walk the FAT chain to a second cluster; routing them
 * through this shared helper fixes that too, as a natural consequence of
 * using one correct implementation everywhere instead of seven hand-rolled
 * ones. */
/*
 * A damaged chain ends the scan (review, found by
 * tests/host/fat32_host.c on corrupted images). The walk used to stop only
 * at an end-of-chain marker, so a FAT entry pointing back into its own chain
 * -- one flipped bit on a card -- looped forever: the shape of "pulling the
 * SD card hangs the board". A chain can hold no more clusters than the
 * volume has, a cluster outside 2..total_clusters+1 is not one, and a sector
 * that does not read is not handed to the callback as whatever the stack
 * held. */
static void fat32_scan_dir(fat32_fs_t *fs, uint32_t start_clus, fat32_dir_scan_fn fn, void *ctx) {
    if (!fs || !fs->dev || !fs->dev->read_blocks || !fn) return;
    uint32_t clus_iter = start_clus;
    for (uint32_t steps = 0; steps <= fs->total_clusters; steps++) {
        if (clus_iter < 2 || clus_iter >= fs->total_clusters + 2) return;
        for (uint32_t s = 0; s < fs->bpb.sec_per_clus; s++) {
            fat32_sector_t sector;
            uint32_t lba = cluster_to_lba(fs, clus_iter) + s;
            if (fs->dev->read_blocks(fs->dev, sector.raw, lba, 1) != 0) return;
            if (fn(fs, lba, sector.entries, 16, ctx)) return;
        }
        clus_iter = fat_get_entry(fs, clus_iter);
        if (clus_iter >= 0x0FFFFFF8) return;
    }
}

static bool dir_entry_is_free(const fat32_dir_entry_t *e) {
    return e->name[0] == 0x00 || (uint8_t)e->name[0] == 0xE5;
}

static inline bool dir_entry_is_corrupt(const fat32_dir_entry_t *e) {
    if ((e->attr & 0x0F) == 0x0F) return false;                  // VFAT LFN metadata
    if ((e->attr & 0xC0) != 0) return true;                      // FAT32 reserved attr bits 6/7 must be 0
    if ((e->attr & FAT32_ATTR_DIRECTORY) && e->file_size != 0) return true; // FAT32: directory size must be 0
    uint8_t c0 = (uint8_t)e->name[0];
    if (c0 != 0x00 && c0 != 0xE5 && c0 != 0x05 && c0 < 0x20) return true; // invalid control char in 8.3 name
    /* Check for illegal characters in short 8.3 name per FAT specification */
    for (int i = 0; i < 11; i++) {
        uint8_t c = (uint8_t)e->name[i];
        if (c < 0x20 && c != 0x00 && c != 0x05) return true;
        if (c == '"' || c == '*' || c == '+' || c == ',' || c == '/' ||
            c == ':' || c == ';' || c == '<' || c == '=' || c == '>' ||
            c == '?' || c == '[' || c == '\\' || c == ']' || c == '|') {
            return true;
        }
    }
    return false;
}

static bool dir_entry_is_skippable(const fat32_dir_entry_t *e) {
    if ((uint8_t)e->name[0] == 0xE5) return true;               // deleted
    if ((e->attr & 0x0F) == 0x0F) return true;                  // VFAT LFN metadata
    if (e->attr & FAT32_ATTR_VOLUME_ID) return true;             // volume label
    if (dir_entry_is_corrupt(e)) return true;                    // corrupted / uninitialized
    return false;
}

static uint32_t entry_first_cluster(const fat32_dir_entry_t *e) {
    return ((uint32_t)e->fst_clus_hi << 16) | e->fst_clus_lo;
}

static bool slot_is_lfn(const fat32_dir_entry_t *e) {
    uint8_t c0 = (uint8_t)e->name[0];
    return (e->attr & 0x0F) == 0x0F && c0 != 0x00 && c0 != 0xE5;
}

/* --- Directory slots -------------------------------------------------------
 *
 * A directory is a chain of 32-byte slots, and a long-name set may cross a
 * sector or a cluster boundary, so slots are walked one at a time. The walker
 * holds one sector; a caller that changes a slot sets `dirty`, and the sector
 * is written back before the walker moves to another one (walk_flush() for
 * the last). */

#define SLOTS_PER_SEC 16u
#define DIR_MAX_SLOTS 65536u        /* FAT's own limit for one directory */

typedef struct {
    fat32_fs_t *fs;
    uint32_t clus, idx;             /* the next slot: its cluster, its index there */
    uint32_t per_clus;              /* slots per cluster */
    uint32_t steps;                 /* clusters entered: a cyclic chain ends the walk */
    uint32_t sec_lba;               /* the sector in `sec`; 0 = none */
    bool dirty, io_error;
    fat32_sector_t sec;
} slot_walk_t;

static void walk_start(slot_walk_t *w, fat32_fs_t *fs, fat32_loc_t at) {
    w->fs = fs;
    w->clus = at.clus;
    w->idx = at.idx;
    w->per_clus = fs->bytes_per_cluster / 32u;
    w->steps = 0;
    w->sec_lba = 0;
    w->dirty = false;
    w->io_error = false;
}

static bool walk_flush(slot_walk_t *w) {
    if (w->dirty && w->sec_lba &&
        w->fs->dev->write_blocks(w->fs->dev, w->sec.raw, w->sec_lba, 1) != 0) {
        w->io_error = true;
    }
    w->dirty = false;
    return !w->io_error;
}

/* The next slot, its place in *at (may be NULL); NULL at the end of the
 * chain, or on a device error (then w->io_error is set). */
static fat32_dir_entry_t *walk_next(slot_walk_t *w, fat32_loc_t *at) {
    fat32_fs_t *fs = w->fs;
    if (w->idx >= w->per_clus) {
        uint32_t next = fat_get_entry(fs, w->clus);
        if (next < 2 || next >= fs->total_clusters + 2 || ++w->steps > fs->total_clusters) return NULL;
        w->clus = next;
        w->idx = 0;
    }
    if (w->clus < 2 || w->clus >= fs->total_clusters + 2) return NULL;
    uint32_t lba = cluster_to_lba(fs, w->clus) + w->idx / SLOTS_PER_SEC;
    if (lba != w->sec_lba) {
        if (!walk_flush(w)) return NULL;
        if (fs->dev->read_blocks(fs->dev, w->sec.raw, lba, 1) != 0) {
            w->sec_lba = 0;
            w->io_error = true;
            return NULL;
        }
        w->sec_lba = lba;
    }
    if (at) {
        at->clus = w->clus;
        at->idx = w->idx;
    }
    return &w->sec.entries[w->idx++ % SLOTS_PER_SEC];
}

/* Slots from the walker's position to the end of its chain. After a 0x00
 * end mark every one of them is free. */
static uint32_t walk_slots_left(slot_walk_t *w) {
    uint32_t n = w->per_clus - w->idx, c = w->clus;
    while (n < DIR_MAX_SLOTS) {
        uint32_t next = fat_get_entry(w->fs, c);
        if (next < 2 || next >= w->fs->total_clusters + 2) break;
        n += w->per_clus;
        c = next;
    }
    return n;
}

static int slot_read(fat32_fs_t *fs, fat32_loc_t at, fat32_dir_entry_t *out) {
    slot_walk_t w;
    walk_start(&w, fs, at);
    fat32_dir_entry_t *s = walk_next(&w, NULL);
    if (!s) return -1;
    *out = *s;
    return 0;
}

static int slot_write(fat32_fs_t *fs, fat32_loc_t at, const fat32_dir_entry_t *e) {
    slot_walk_t w;
    walk_start(&w, fs, at);
    fat32_dir_entry_t *s = walk_next(&w, NULL);
    if (!s) return -1;
    *s = *e;
    w.dirty = true;
    return walk_flush(&w) ? 0 : -1;
}

/* --- Long-name sets ------------------------------------------------------- */

typedef struct {
    uint8_t seq;                    /* ordinal of the last long-name slot read; 0 = no set open */
    uint8_t n, sum;
    bool match;                     /* the set so far spells the wanted name */
    uint32_t units;                 /* the set's name length */
    fat32_loc_t start;              /* its first slot */
} lfn_track_t;

static uint16_t lfn_unit(const fat32_dir_entry_t *e, uint32_t i) {
    const uint8_t *b = (const uint8_t *)e;
    return (uint16_t)(b[k_lfn_off[i]] | (b[k_lfn_off[i] + 1] << 8));
}

/* Whether slot `e`, ordinal `ord`, spells units (ord-1)*13 .. of `name`.
 * Past the name's end a slot holds a 0x0000 terminator and 0xFFFF padding;
 * either is accepted there. */
static bool lfn_chunk_matches(const fat32_dir_entry_t *e, uint32_t ord, const char *name, size_t len) {
    u16_iter_t it;
    u16_init(&it, name, len);
    for (uint32_t k = 0; k < (ord - 1) * LFN_UNITS; k++) {
        if (u16_next(&it) < 0) return false;
    }
    bool ended = false;
    for (uint32_t i = 0; i < LFN_UNITS; i++) {
        uint16_t have = lfn_unit(e, i);
        int32_t want = ended ? -1 : u16_next(&it);
        if (want < 0) {
            if (have != 0x0000 && have != 0xFFFF) return false;
            ended = true;
        } else if (fold(have) != fold((uint32_t)want)) {
            return false;
        }
    }
    return true;
}

/* Feeds one long-name slot. Slots of a set come last part first, ordinals
 * counting down to 1; anything out of that order drops the set, and an 8.3
 * entry then stands alone (an orphaned set is ignored, as Windows does).
 * `want` may be NULL; `cap` (LFN_MAX_SLOTS * LFN_UNITS units) may be NULL. */
static void lfn_feed(lfn_track_t *t, const fat32_dir_entry_t *e, fat32_loc_t at,
                     const char *want, size_t want_len, int32_t want_units, uint16_t *cap) {
    uint8_t ord = (uint8_t)e->name[0];
    uint8_t sum = ((const uint8_t *)e)[13];
    if (ord & 0x40u) {
        uint8_t n = ord & 0x1Fu;
        uint32_t k = 0;
        while (k < LFN_UNITS && lfn_unit(e, k) != 0x0000 && lfn_unit(e, k) != 0xFFFF) k++;
        if (n == 0 || n > LFN_MAX_SLOTS || (n == 1 && k == 0)) {
            t->seq = 0;
            return;
        }
        t->seq = n;
        t->n = n;
        t->sum = sum;
        t->start = at;
        t->units = (uint32_t)(n - 1u) * LFN_UNITS + k;
        t->match = want && want_units >= 0 && t->units == (uint32_t)want_units;
    } else if (t->seq > 1 && ord == t->seq - 1u && sum == t->sum) {
        t->seq = ord;
    } else {
        t->seq = 0;
        return;
    }
    if (t->match) t->match = lfn_chunk_matches(e, t->seq, want, want_len);
    if (cap) {
        for (uint32_t i = 0; i < LFN_UNITS; i++) cap[(t->seq - 1u) * LFN_UNITS + i] = lfn_unit(e, i);
    }
}

/* Whether the set just read belongs to 8.3 entry `e`, the slot after it. */
static bool lfn_owns(const lfn_track_t *t, const fat32_dir_entry_t *e) {
    return t->seq == 1 && t->sum == lfn_checksum(e->name);
}

/* Captured units as UTF-8; false when they do not fit `max` or hold a lone
 * surrogate (the caller then shows the alias, which always opens the file). */
static bool lfn_to_utf8(const uint16_t *u, uint32_t units, char *out, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < units; i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            c = 0x10000u + ((c - 0xD800u) << 10) + (u[++i] - 0xDC00u);
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            return false;
        }
        if (c == 0 || !utf8_put(out, max, &n, c)) return false;
    }
    out[n] = '\0';
    return true;
}

static void lfn_fill(fat32_dir_entry_t *s, const char *name, size_t len, uint32_t ord, bool last, uint8_t sum) {
    uint8_t *b = (uint8_t *)s;
    memset(b, 0, 32);
    b[0] = (uint8_t)(ord | (last ? 0x40u : 0u));
    b[11] = 0x0F;
    b[13] = sum;
    u16_iter_t it;
    u16_init(&it, name, len);
    for (uint32_t k = 0; k < (ord - 1) * LFN_UNITS; k++) (void)u16_next(&it);
    bool ended = false;
    for (uint32_t i = 0; i < LFN_UNITS; i++) {
        int32_t u = ended ? -1 : u16_next(&it);
        uint16_t v;
        if (u >= 0) {
            v = (uint16_t)u;
        } else if (!ended) {
            v = 0x0000;
            ended = true;
        } else {
            v = 0xFFFF;
        }
        b[k_lfn_off[i]] = (uint8_t)v;
        b[k_lfn_off[i] + 1] = (uint8_t)(v >> 8);
    }
}

/* --- Searching a directory -------------------------------------------------
 *
 * One pass over a directory answers everything a lookup or a create needs:
 * whether the name is there (as a long name or as an 8.3 name, either case),
 * where its whole set is, where a new set of `need` slots fits, and which
 * alias candidates are taken. */

typedef struct {
    /* in */
    const char *name;               /* the wanted name, not NUL-terminated; NULL matches nothing */
    size_t len;
    int32_t units;                  /* name_count_units() of it */
    bool has_skip;
    fat32_loc_t skip;               /* an entry that does not count: a rename's own source */
    uint32_t need;                  /* free slots for a new set; 0 = not creating */
    char (*cand)[11];
    uint32_t ncand;
    /* out */
    bool found;
    fat32_dir_entry_t entry;
    fat32_loc_t at;                 /* the 8.3 entry */
    fat32_loc_t set;                /* the set's first slot: its long-name slots, or `at` */
    uint32_t set_len;
    fat32_loc_t run;                /* `need` free slots in a row, or the chain's free tail */
    uint32_t run_len;               /* < need: only the tail; the directory must grow */
    uint16_t cand_used;             /* bit k: cand[k] is some entry's 8.3 name */
    bool io_error;
} dir_find_t;

static void dir_find(fat32_fs_t *fs, uint32_t dir_clus, dir_find_t *f) {
    slot_walk_t w;
    fat32_loc_t at = { dir_clus, 0 }, run_at = at;
    walk_start(&w, fs, at);
    lfn_track_t t;
    memset(&t, 0, sizeof t);
    uint32_t run = 0;
    bool run_done = f->need == 0;
    f->found = false;
    f->run = at;
    f->run_len = 0;
    f->cand_used = 0;
    f->io_error = false;

    fat32_dir_entry_t *e;
    while ((e = walk_next(&w, &at)) != NULL) {
        uint8_t c0 = (uint8_t)e->name[0];
        bool lfn = slot_is_lfn(e);
        bool free_slot = c0 == 0x00 || c0 == 0xE5 || (!lfn && dir_entry_is_corrupt(e));
        if (!run_done) {
            if (!free_slot) {
                run = 0;
            } else if (run++ == 0) {
                run_at = at;
            }
            if (run >= f->need) {
                f->run = run_at;
                f->run_len = run;
                run_done = true;
            }
        }
        if (c0 == 0x00) {                           /* the end: the rest is free */
            if (!run_done) {
                f->run = run_at;
                f->run_len = run + walk_slots_left(&w);
            }
            return;
        }
        if (lfn) {
            lfn_feed(&t, e, at, f->name, f->len, f->units, NULL);
            continue;
        }
        bool owned = lfn_owns(&t, e);
        t.seq = 0;
        if (free_slot || dir_entry_is_skippable(e)) continue;
        for (uint32_t k = 0; k < f->ncand; k++) {
            if (memcmp(e->name, f->cand[k], 11) == 0) f->cand_used |= (uint16_t)(1u << k);
        }
        if (!f->name) continue;
        if (f->has_skip && at.clus == f->skip.clus && at.idx == f->skip.idx) continue;
        if ((owned && t.match) || short_matches(e, f->name, f->len)) {
            f->found = true;
            f->entry = *e;
            f->at = at;
            f->set = owned ? t.start : at;
            f->set_len = owned ? t.n + 1u : 1u;
            return;
        }
    }
    if (w.io_error) {
        f->io_error = true;
        return;
    }
    if (!run_done) {                                /* a full chain, no end mark */
        f->run = run_at;
        f->run_len = run;
    }
}

/* Appends zeroed clusters to directory `dir_clus` until the free tail found
 * by dir_find() holds f->need slots. A cluster whose FAT entry is free must
 * never be linked to (the allocator could hand it out again), so only a
 * chain that ends in a proper end-of-chain mark is extended. */
static int dir_grow(fat32_fs_t *fs, uint32_t dir_clus, dir_find_t *f) {
    uint32_t last = dir_clus;
    bool found_end = false;
    for (uint32_t guard = 0; guard < DIR_MAX_SLOTS; guard++) {
        uint32_t next = fat_get_entry(fs, last);
        if (next >= 0x0FFFFFF8) { found_end = true; break; }
        if (next < 2 || next >= fs->total_clusters + 2) break;
        last = next;
    }
    const char *dn = fs->dev->name ? fs->dev->name : "unknown";
    if (!found_end) {
        printk("[FAT32] Device '%s': directory cluster chain is damaged or does not end; "
               "refusing to extend it\n", dn);
        return -1;
    }
    uint32_t per = fs->bytes_per_cluster / 32u;
    uint32_t more = (f->need - f->run_len + per - 1u) / per;
    fat32_sector_t zero;
    memset(&zero, 0, sizeof zero);
    for (uint32_t i = 0; i < more; i++) {
        uint32_t clus = fat_alloc_cluster(fs);
        if (clus == 0 || clus == last) {
            if (clus == 0 && fs->alloc_io_error)
                printk("[FAT32] Device '%s': cannot extend a directory, the FAT does not read "
                       "(card removed?)\n", dn);
            else
                printk("[FAT32] Device '%s': directory is full and the volume has no free "
                       "cluster to extend it with\n", dn);
            return -1;
        }
        uint32_t base = cluster_to_lba(fs, clus);
        for (uint32_t s = 0; s < fs->bpb.sec_per_clus; s++) {
            if (fs->dev->write_blocks(fs->dev, zero.raw, base + s, 1) != 0) {
                fat32_free_chain(fs, clus);
                return -1;
            }
        }
        fat_set_entry(fs, last, clus);
        if (f->run_len == 0 && i == 0) {
            f->run.clus = clus;
            f->run.idx = 0;
        }
        last = clus;
    }
    f->run_len += more * per;
    return 0;
}

/* Writes a set at `run`: `nlfn` long-name slots spelling `name`, then `e`.
 * *at_out (may be NULL) gets the 8.3 entry's place. */
static int dir_write_set(fat32_fs_t *fs, fat32_loc_t run, const char *name, size_t len,
                         uint32_t nlfn, const fat32_dir_entry_t *e, fat32_loc_t *at_out) {
    slot_walk_t w;
    walk_start(&w, fs, run);
    uint8_t sum = lfn_checksum(e->name);
    for (uint32_t i = 0; i <= nlfn; i++) {
        fat32_loc_t at;
        fat32_dir_entry_t *s = walk_next(&w, &at);
        if (!s) return -1;
        if (i < nlfn) {
            lfn_fill(s, name, len, nlfn - i, i == 0, sum);
        } else {
            *s = *e;
            if (at_out) *at_out = at;
        }
        w.dirty = true;
    }
    return walk_flush(&w) ? 0 : -1;
}

/* Marks a whole set deleted, its long-name slots with its 8.3 entry: left
 * behind, they would be read as an orphan, or, once their slots' neighbours
 * are reused, attach a stale long name to some other file. */
static int dir_erase_set(fat32_fs_t *fs, fat32_loc_t set, uint32_t n) {
    slot_walk_t w;
    walk_start(&w, fs, set);
    for (uint32_t i = 0; i < n; i++) {
        fat32_dir_entry_t *s = walk_next(&w, NULL);
        if (!s) return -1;
        s->name[0] = (char)0xE5;
        w.dirty = true;
    }
    return walk_flush(&w) ? 0 : -1;
}

/* `name` NUL-terminated for a log line (cut at 63 bytes): the kernel's
 * formatter has no "%.*s". */
static const char *log_name(char out[64], const char *name, size_t len) {
    size_t n = len < 63u ? len : 63u;
    memcpy(out, name, n);
    out[n] = '\0';
    return out;
}

/* Sets up *f for a search that is also ready to create `name`: the free
 * slots its set needs and, for a name that is not 8.3, round `round`'s alias
 * candidates in `cand`. Returns the name's length in units, or -1 when no
 * file may have it (then *f still finds an existing entry by it). */
static int32_t create_query(dir_find_t *f, const char *name, size_t len, uint32_t round,
                            char cand[ALIAS_MAX][11]) {
    memset(f, 0, sizeof *f);
    f->name = name;
    f->len = len;
    f->units = name_count_units(name, len);
    int32_t units = name_units(name, len);
    if (units < 0) return -1;
    char short_name[11];
    uint8_t nt;
    if (name_fits_83(name, len, short_name, &nt)) {
        f->need = 1;
    } else {
        f->need = ((uint32_t)units + LFN_UNITS - 1u) / LFN_UNITS + 1u;
        f->ncand = alias_candidates(name, len, round, cand);
        f->cand = cand;
    }
    return units;
}

/* Adds an entry named `name` to directory `dir_clus`; every field but the
 * name comes from *proto. `pre` (may be NULL) is a create_query() search of
 * this directory for this name, round 0, already run and not found: creating
 * then costs no second pass over the directory. Returns 0, -2 if the name is
 * already there (apart from `skip`, may be NULL), -1 on an invalid name, a
 * full volume or a device error. *at_out (may be NULL) gets the new 8.3
 * entry's place. */
static int dir_create(fat32_fs_t *fs, uint32_t dir_clus, const char *name, size_t len,
                      const fat32_dir_entry_t *proto, const fat32_loc_t *skip, fat32_loc_t *at_out,
                      dir_find_t *pre) {
    fat32_dir_entry_t e = *proto;
    char short_name[11];
    uint8_t nt = 0;
    bool fits = name_fits_83(name, len, short_name, &nt);
    char cand[ALIAS_MAX][11];

    for (uint32_t round = 0; round < ALIAS_ROUNDS; round++) {
        dir_find_t f;
        if (round == 0 && pre) {
            f = *pre;
        } else {
            if (create_query(&f, name, len, round, cand) < 0) {
                char ln[64];
                printk("[FAT32] Device '%s': '%s' is not a valid file name\n",
                       fs->dev->name ? fs->dev->name : "unknown", log_name(ln, name, len));
                return -1;
            }
            if (skip) {
                f.has_skip = true;
                f.skip = *skip;
            }
            dir_find(fs, dir_clus, &f);
        }
        if (f.io_error) return -1;
        if (f.found) return -2;
        if (fits) {
            memcpy(e.name, short_name, 11);
            e.nt_res = nt;
        } else {
            uint32_t k = 0;
            while (k < f.ncand && (f.cand_used & (1u << k))) k++;
            if (k == f.ncand) continue;
            memcpy(e.name, f.cand[k], 11);
            e.nt_res = 0;
        }
        if (f.run_len < f.need && dir_grow(fs, dir_clus, &f) != 0) return -1;
        return dir_write_set(fs, f.run, name, len, f.need - 1u, &e, at_out);
    }
    char ln[64];
    printk("[FAT32] Device '%s': no free short alias for '%s'\n",
           fs->dev->name ? fs->dev->name : "unknown", log_name(ln, name, len));
    return -1;
}

/* --- Paths ----------------------------------------------------------------- */

/* The directory holding `path`'s last component, which *name / *len get (not
 * NUL-terminated; empty for the root itself). 0 if a directory on the way is
 * missing or is not one. Trailing separators are dropped: "system/" names
 * "system", and whether it must then be a directory is lookup()'s check. */
static uint32_t path_parent(fat32_fs_t *fs, const char *path, const char **name, size_t *len) {
    while (*path == '/') path++;
    size_t plen = strlen(path);
    while (plen > 0 && path[plen - 1] == '/') plen--;
    const char *p = path, *end = path + plen;
    uint32_t clus = fs->root_dir_cluster;
    for (;;) {
        const char *s = memchr(p, '/', (size_t)(end - p));
        if (!s) {
            *name = p;
            *len = (size_t)(end - p);
            return clus;
        }
        if (s > p) {
            dir_find_t f;
            memset(&f, 0, sizeof f);
            f.name = p;
            f.len = (size_t)(s - p);
            f.units = name_count_units(p, f.len);
            if (f.units < 0) return 0;
            dir_find(fs, clus, &f);
            if (!f.found || !(f.entry.attr & FAT32_ATTR_DIRECTORY)) return 0;
            uint32_t c = entry_first_cluster(&f.entry);
            clus = c ? c : fs->root_dir_cluster;
        }
        p = s + 1;
    }
}

/* Finds `path` (not the root) into *f; *parent (may be NULL) gets the
 * directory it is in. */
static int lookup(fat32_fs_t *fs, const char *path, dir_find_t *f, uint32_t *parent) {
    const char *name;
    size_t len;
    memset(f, 0, sizeof *f);
    uint32_t pc = path_parent(fs, path, &name, &len);
    if (pc == 0 || len == 0) return -1;
    f->name = name;
    f->len = len;
    f->units = name_count_units(name, len);
    if (f->units < 0) return -1;
    dir_find(fs, pc, f);
    if (!f->found) return -1;
    /* "file.txt/" is not file.txt: a trailing separator asks for a directory. */
    size_t plen = strlen(path);
    if (plen > 0 && path[plen - 1] == '/' && !(f->entry.attr & FAT32_ATTR_DIRECTORY)) return -1;
    if (parent) *parent = pc;
    return 0;
}

int fat32_format(block_dev_t *dev) {
    return fat32_format_quiet(dev, false);
}

/* The layout is sized from the volume. It used
 * to be fixed -- 512-byte clusters and an 8-sector FAT -- which addresses
 * 1022 clusters, 511 KB: the whole of every RAM disk while the cap was
 * 512 KB, and a quarter of the LCD-7's 2 MB one, which mounted at "75 %
 * used" while empty. fat32_init() clamps to what the FAT addresses, so
 * nothing was corrupted; the space was simply unreachable.
 *
 * Clusters stay one sector until a volume would need more than 65 536 of
 * them, then double (up to 32 KB): a FAT of at most 512 sectors per copy for
 * anything up to 2 GB, and every FAT sector is zeroed -- a reformat of a used
 * volume no longer inherits the old chains past the first FAT sector. */
int fat32_format_quiet(block_dev_t *dev, bool quiet) {
    if (!dev || !dev->write_blocks) return -1;
    const uint32_t reserved = 32;
    const uint32_t num_fats = 2;
    if (dev->num_blocks <= reserved + num_fats + 1) return -1;

    uint32_t spc = 1;
    while (spc < 64 && (dev->num_blocks - reserved) / spc > 65536u) spc <<= 1;
    /* Entries for every cluster the data area could hold if the FATs took no
     * room -- an upper bound, so the FAT is never too small. */
    uint32_t fat_sz = (((dev->num_blocks - reserved) / spc + 2u) * 4u + 511u) / 512u;
    uint32_t data_start = reserved + num_fats * fat_sz;
    if (data_start + spc > dev->num_blocks) return -1;

    fat32_sector_t sector;
    memset(&sector, 0, sizeof(sector));

    fat32_bpb_t *bpb = &sector.bpb;
    bpb->jmp_boot[0] = 0xEB; bpb->jmp_boot[1] = 0x58; bpb->jmp_boot[2] = 0x90;
    memcpy(bpb->oem_name, "MSWIN4.1", 8);
    bpb->bytes_per_sec = 512;
    bpb->sec_per_clus = (uint8_t)spc;
    bpb->reserved_sec_cnt = (uint16_t)reserved;
    bpb->num_fats = (uint8_t)num_fats;
    bpb->tot_sec32 = dev->num_blocks;
    bpb->fat_sz32 = fat_sz;
    bpb->root_clus = 2;
    bpb->boot_sig = 0x29;
    memcpy(bpb->vol_lab, "LUGALOS_FAT", 11);
    memcpy(bpb->fil_sys_type, "FAT32   ", 8);
    sector.raw[510] = 0x55;
    sector.raw[511] = 0xAA;

    if (dev->write_blocks(dev, sector.raw, 0, 1) != 0) return -1;

    /* Both FATs and the root directory's cluster, zeroed... */
    memset(&sector, 0, sizeof(sector));
    for (uint32_t lba = reserved; lba < data_start + spc; lba++) {
        if (dev->write_blocks(dev, sector.raw, lba, 1) != 0) return -1;
    }

    /* ...then each FAT's first sector: the media and end-of-chain entries,
     * and cluster 2, the root directory, a chain of one. */
    sector.words[0] = 0x0FFFFFF8;
    sector.words[1] = 0x0FFFFFFF;
    sector.words[2] = 0x0FFFFFFF;
    for (uint32_t f = 0; f < num_fats; f++) {
        if (dev->write_blocks(dev, sector.raw, reserved + f * fat_sz, 1) != 0) return -1;
    }

    if (!quiet) {
        printk("[FAT32] Device '%s': Volume formatted cleanly as FAT32.\n", dev->name ? dev->name : "unknown");
    }
    return 0;
}

/* --- fat32_mkfs ---------------------------------------------------------- */

#define MKFS_ALIGN      8192u      /* 4 MB in sectors: the partition start and the data area */
#define MKFS_MIN_CLUST  65525u     /* fewer clusters is FAT16 by definition */

int fat32_mkfs(block_dev_t *dev, const char *label, uint32_t vol_id, void (*progress)(uint32_t pct)) {
    if (!dev || !dev->write_blocks) return -1;
    const uint32_t total = dev->num_blocks;
    if (total <= MKFS_ALIGN * 2u) return -1;
    const uint32_t part_start = MKFS_ALIGN;
    const uint32_t part_size = total - part_start;

    /* Largest cluster (sectors, a power of two up to 64) that still leaves
     * enough clusters; and the FAT and reserved sizes that put the data area
     * on the alignment boundary. */
    uint32_t spc = 64, reserved = 0, fat_sz = 0, clusters = 0;
    for (; spc >= 1; spc >>= 1) {
        /* The FAT sized for the most clusters the partition could hold; the
         * padding that aligns the data area then only takes clusters away,
         * so the FAT is never short (fsck.fat checks exactly that). */
        uint32_t most = (part_size - 32u) / spc;
        fat_sz = ((most + 2u) * 4u + 511u) / 512u;
        uint32_t data_abs = part_start + 32u + 2u * fat_sz;
        reserved = 32u + (MKFS_ALIGN - data_abs % MKFS_ALIGN) % MKFS_ALIGN;
        clusters = (part_size - reserved - 2u * fat_sz) / spc;
        if (clusters >= MKFS_MIN_CLUST) break;
    }
    if (spc < 1 || clusters < MKFS_MIN_CLUST) return -1;
    const uint32_t data_rel = reserved + 2u * fat_sz;

    char lab[11];
    memset(lab, ' ', sizeof lab);
    const char *l = label && *label ? label : "LUGALOS";
    for (int i = 0; i < 11 && l[i]; i++) {
        char c = l[i];
        lab[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }

    fat32_sector_t s;
    /* The MBR: one primary partition, FAT32 with LBA, no boot code. */
    memset(&s, 0, sizeof s);
    s.raw[0x1B8] = (uint8_t)vol_id; s.raw[0x1B9] = (uint8_t)(vol_id >> 8);
    s.raw[0x1BA] = (uint8_t)(vol_id >> 16); s.raw[0x1BB] = (uint8_t)(vol_id >> 24);
    uint8_t *pe = &s.raw[0x1BE];
    pe[0] = 0x00;                                  /* not bootable */
    pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;      /* CHS: "use the LBA fields" */
    pe[4] = 0x0C;                                  /* FAT32, LBA */
    pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
    memcpy(&pe[8], &part_start, 4);
    memcpy(&pe[12], &part_size, 4);
    s.raw[510] = 0x55; s.raw[511] = 0xAA;
    if (dev->write_blocks(dev, s.raw, 0, 1) != 0) return -1;

    /* The volume boot record, at the partition's sector 0 and 6. */
    memset(&s, 0, sizeof s);
    fat32_bpb_t *b = &s.bpb;
    b->jmp_boot[0] = 0xEB; b->jmp_boot[1] = 0x58; b->jmp_boot[2] = 0x90;
    memcpy(b->oem_name, "MSWIN4.1", 8);
    b->bytes_per_sec = 512;
    b->sec_per_clus = (uint8_t)spc;
    b->reserved_sec_cnt = (uint16_t)reserved;
    b->num_fats = 2;
    b->media = 0xF8;
    b->sec_per_trk = 63;
    b->num_heads = 255;
    b->hidd_sec = part_start;
    b->tot_sec32 = part_size;
    b->fat_sz32 = fat_sz;
    b->root_clus = 2;
    b->fs_info = 1;
    b->bk_boot_sec = 6;
    b->drv_num = 0x80;
    b->boot_sig = 0x29;
    b->vol_id = vol_id;
    memcpy(b->vol_lab, lab, 11);
    memcpy(b->fil_sys_type, "FAT32   ", 8);
    s.raw[510] = 0x55; s.raw[511] = 0xAA;
    if (dev->write_blocks(dev, s.raw, part_start, 1) != 0) return -1;
    if (dev->write_blocks(dev, s.raw, part_start + 6u, 1) != 0) return -1;

    /* FSInfo at 1 and 7: every cluster but the root's free, the next from 3. */
    memset(&s, 0, sizeof s);
    s.words[FSINFO_W_LEAD] = FSINFO_LEAD_SIG;
    s.words[FSINFO_W_STRUC] = FSINFO_STRUC_SIG;
    s.words[FSINFO_W_FREE] = clusters - 1u;
    s.words[FSINFO_W_NXT] = 3;
    s.words[FSINFO_W_TRAIL] = FSINFO_TRAIL_SIG;
    if (dev->write_blocks(dev, s.raw, part_start + 1u, 1) != 0) return -1;
    if (dev->write_blocks(dev, s.raw, part_start + 7u, 1) != 0) return -1;

    /* The rest of the reserved area (a stale backup boot sector or FSInfo
     * from an earlier format must not survive), both FATs, the root cluster. */
    memset(&s, 0, sizeof s);
    for (uint32_t r = 2; r < reserved; r++) {
        if (r == 6 || r == 7) continue;
        if (dev->write_blocks(dev, s.raw, part_start + r, 1) != 0) return -1;
    }
    uint32_t zero_end = data_rel + spc, last_pct = 101;
    for (uint32_t r = reserved; r < zero_end; r++) {
        if (dev->write_blocks(dev, s.raw, part_start + r, 1) != 0) return -1;
        uint32_t pct = (uint32_t)((uint64_t)(r - reserved) * 100u / (zero_end - reserved));
        if (progress && pct != last_pct && pct % 10u == 0) { progress(pct); last_pct = pct; }
    }
    /* Each FAT's first sector: media, end-of-chain, the root a chain of one. */
    s.words[0] = 0x0FFFFFF8;
    s.words[1] = 0x0FFFFFFF;
    s.words[2] = 0x0FFFFFFF;
    for (uint32_t f = 0; f < 2u; f++) {
        if (dev->write_blocks(dev, s.raw, part_start + reserved + f * fat_sz, 1) != 0) return -1;
    }
    /* The root directory's first entry: the volume label. */
    memset(&s, 0, sizeof s);
    memcpy(s.entries[0].name, lab, 11);
    s.entries[0].attr = FAT32_ATTR_VOLUME_ID;
    if (dev->write_blocks(dev, s.raw, part_start + data_rel, 1) != 0) return -1;
    if (progress) progress(100);
    return 0;
}

int fat32_init(fat32_fs_t *fs, block_dev_t *dev) {
    return fat32_init_quiet(fs, dev, false);
}

int fat32_init_quiet(fat32_fs_t *fs, block_dev_t *dev, bool quiet) {
    if (!fs || !dev || !dev->read_blocks) return -1;
    fs->dev = dev;

    uint8_t sector[512];
    if (dev->read_blocks(dev, sector, 0, 1) != 0) {
        if (!quiet) printk("[FAT32] Device '%s': boot sector unreadable (not mounted)\n",
                           dev->name ? dev->name : "unknown");
        return -1;
    }

    uint32_t partition_lba = 0;

    /* Check if LBA 0 is an MBR Master Boot Record with partition table at offset 0x1BE */
    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        uint8_t *part1 = &sector[0x01BE];
        uint8_t part_type = part1[4];
        uint32_t start_lba = (uint32_t)part1[8] | ((uint32_t)part1[9] << 8) |
                             ((uint32_t)part1[10] << 16) | ((uint32_t)part1[11] << 24);

        if ((part_type == 0x0B || part_type == 0x0C || part_type == 0x07 || part_type == 0x0E) && start_lba > 0 && start_lba < 0x0FFFFFFF) {
            partition_lba = start_lba;
            if (dev->read_blocks(dev, sector, partition_lba, 1) != 0) {
                if (!quiet) printk("[FAT32] Device '%s': partition boot sector unreadable\n",
                                   dev->name ? dev->name : "unknown");
                return -1;
            }
            printk("[FAT32] Device '%s': Parsed MBR Partition 1 at LBA %u (Type 0x%02X)\n",
                   dev->name ? dev->name : "unknown", (unsigned int)partition_lba, part_type);
        }
    }

    memcpy(&fs->bpb, sector, sizeof(fat32_bpb_t));

    if (fs->bpb.boot_sig != 0x29 && sector[510] != 0x55) {
        /* Used to auto-format here on any unrecognized boot sector -- which
         * meant inserting a blank, foreign-formatted, or merely corrupt SD
         * card silently wiped it (see B10 in
         * plan/completed/2026-08-07_review_and_remediation.md). Report the volume as
         * unmounted instead; fat32_format() is still available and is now
         * only ever invoked explicitly (via the `format` Lisp primitive,
         * or vfs_mount_ramdisk()'s own deliberate fallback for the RAM
         * disk, which is expected to start blank every boot). */
        if (!quiet) {
            printk("[FAT32] Device '%s': No valid FAT32 volume found (not mounted). "
                   "Use (format \"<path>\") to initialize it.\n",
                   dev->name ? dev->name : "unknown");
        }
        return -1;
    }

    fs->fat_start_sector = partition_lba + fs->bpb.reserved_sec_cnt;
    fs->data_start_sector = fs->fat_start_sector + (fs->bpb.num_fats * fs->bpb.fat_sz32);
    fs->root_dir_cluster = fs->bpb.root_clus;
    fs->bytes_per_cluster = fs->bpb.sec_per_clus * fs->bpb.bytes_per_sec;

    /* 38.0: the allocator's bookkeeping. A remount (format, then init again
     * on the same struct) must not keep anything from the old volume, but it
     * keeps the cache buffer itself -- that belongs to the mount, not to the
     * volume -- and the lock, which nobody can hold during a mount. */
    uint32_t total_sec = fs->bpb.tot_sec32 ? fs->bpb.tot_sec32 : fs->bpb.tot_sec16;
    uint32_t fat_area_sec = fs->bpb.reserved_sec_cnt + (uint32_t)fs->bpb.num_fats * fs->bpb.fat_sz32;
    fs->total_clusters = (fs->bpb.sec_per_clus && total_sec > fat_area_sec)
                       ? (total_sec - fat_area_sec) / fs->bpb.sec_per_clus : 0;
    /* A FAT holds fat_sz32 * 128 entries, two of them reserved. A volume
     * that claims more clusters than its FAT can describe is damaged; never
     * let the allocator index past the FAT on its say-so. */
    uint32_t fat_entries = fs->bpb.fat_sz32 * 128u;
    if (fat_entries >= 2 && fs->total_clusters > fat_entries - 2) fs->total_clusters = fat_entries - 2;

    /* A boot sector this driver cannot use is refused, with the reason
     *. Everything here assumes 512-byte sectors and walks
     * the FAT from fat_sz32, so a card with 4 KB sectors, or a FAT16 volume,
     * used to mount and then read the wrong bytes everywhere; a cluster size
     * of 0, no FAT, or a root directory outside the volume mounted a
     * filesystem on which nothing worked. tests/host/fat32_host.c mounts
     * corrupted images to keep this honest. */
    const char *why = NULL;
    uint8_t spc = fs->bpb.sec_per_clus;
    if (fs->bpb.bytes_per_sec != 512)              why = "sector size is not 512 bytes";
    else if (spc == 0 || (spc & (spc - 1)) != 0)   why = "cluster size is not a power of two";
    else if (fs->bpb.fat_sz32 == 0)                why = "not FAT32 (FAT12/16, or no FAT size)";
    else if (fs->bpb.num_fats == 0)                why = "no FAT copy";
    else if (fs->bpb.reserved_sec_cnt == 0)        why = "no reserved sectors";
    else if (fs->total_clusters == 0)              why = "no data clusters";
    else if (fs->root_dir_cluster < 2 || fs->root_dir_cluster >= fs->total_clusters + 2)
                                                   why = "root directory cluster outside the volume";
    if (why) {
        if (!quiet) printk("[FAT32] Device '%s': not mounted: %s\n",
                           dev->name ? dev->name : "unknown", why);
        return -1;
    }
    fs->free_count = FAT32_FREE_UNKNOWN;
    fs->next_free = 2;
    fs->fsinfo_lba = 0;
    fs->fsinfo_valid = false;
    fs->modified = false;
    fs->gen++;
    fs->fat_cache_lba = 0;
    if (fs->bpb.fs_info >= 1 && fs->bpb.fs_info < fs->bpb.reserved_sec_cnt) {
        fat32_sector_t info;
        uint32_t lba = partition_lba + fs->bpb.fs_info;
        if (dev->read_blocks(dev, info.raw, lba, 1) == 0 &&
            info.words[FSINFO_W_LEAD] == FSINFO_LEAD_SIG &&
            info.words[FSINFO_W_STRUC] == FSINFO_STRUC_SIG &&
            info.words[FSINFO_W_TRAIL] == FSINFO_TRAIL_SIG) {
            fs->fsinfo_lba = lba;
            uint32_t fc = info.words[FSINFO_W_FREE];
            uint32_t nf = info.words[FSINFO_W_NXT];
            if (fc <= fs->total_clusters) {
                fs->free_count = fc;
                fs->fsinfo_valid = true;
            }
            if (nf >= 2 && nf < fs->total_clusters + 2) fs->next_free = nf;
        }
    }

    if (!quiet) {
        printk("[FAT32] Device '%s': Volume Mounted. Label: '%.11s', Data LBA: %u\n",
               dev->name ? dev->name : "unknown", fs->bpb.vol_lab, (unsigned int)fs->data_start_sector);
    }
    return 0;
}


/* --- fat32_find_file / fat32_lookup --- */

static int fat32_lookup_nolock(fat32_fs_t *fs, const char *path, fat32_dir_entry_t *out_entry, fat32_loc_t *loc) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks) return -1;
    while (*path == '/') path++;
    if (*path == '\0') {
        if (out_entry) {
            memset(out_entry, 0, sizeof(fat32_dir_entry_t));
            out_entry->attr = FAT32_ATTR_DIRECTORY;
            out_entry->fst_clus_hi = (uint16_t)(fs->root_dir_cluster >> 16);
            out_entry->fst_clus_lo = (uint16_t)(fs->root_dir_cluster & 0xFFFF);
        }
        if (loc) loc->clus = loc->idx = 0;
        return 0;
    }
    dir_find_t f;
    if (lookup(fs, path, &f, NULL) != 0) return -1;
    if (out_entry) *out_entry = f.entry;
    if (loc) *loc = f.at;
    return 0;
}

int fat32_lookup(fat32_fs_t *fs, const char *path, fat32_dir_entry_t *out_entry, fat32_loc_t *loc) {
    if (!fs) return -1;
    ylock_acquire(&fs->lock);
    int r = fat32_lookup_nolock(fs, path, out_entry, loc);
    ylock_release(&fs->lock);
    return r;
}

int fat32_find_file(fat32_fs_t *fs, const char *path, fat32_dir_entry_t *out_entry) {
    return fat32_lookup(fs, path, out_entry, NULL);
}

/* The cluster holding cluster-index `target` of the chain starting at
 * `first`, starting from `cur` when it is still valid and not past the
 * target, else from the first cluster. With `extend`, a chain that ends
 * early is grown with zeroed clusters (a write past EOF zero-fills the gap);
 * without, an early end returns 0. Updates `cur` to the result. */
static uint32_t chain_seek(fat32_fs_t *fs, uint32_t first, uint32_t target,
                           fat32_cursor_t *cur, bool extend) {
    uint32_t cluster = first, index = 0;
    if (cur && cur->first == first && cur->gen == fs->gen &&
        cur->cluster >= 2 && cur->index <= target) {
        cluster = cur->cluster;
        index = cur->index;
    }
    while (index < target) {
        uint32_t next = fat_get_entry(fs, cluster);
        if (next >= 0x0FFFFFF8) {
            if (!extend) return 0;
            uint32_t new_clus = fat_alloc_cluster(fs);
            if (new_clus == 0) return 0;
            fat_set_entry(fs, cluster, new_clus);
            uint8_t zero_sec[512];
            memset(zero_sec, 0, 512);
            for (uint32_t s = 0; s < fs->bpb.sec_per_clus; s++) {
                fs->dev->write_blocks(fs->dev, zero_sec, cluster_to_lba(fs, new_clus) + s, 1);
            }
            next = new_clus;
        }
        if (next < 2) return 0;   /* a free or reserved entry inside a chain: damaged */
        cluster = next;
        index++;
    }
    if (cur) {
        cur->first = first;
        cur->gen = fs->gen;
        cur->index = index;
        cur->cluster = cluster;
    }
    return cluster;
}

/* Reads `count` bytes starting at byte `offset` within the file described
 * by `entry`. With a cursor, continues from where the handle last was in the
 * chain (38.0); without one, walks from the first cluster, which is O(offset).
 * Unlike fat32_read_file(), does not NUL-terminate: this is the byte-exact
 * primitive underlying fs/vfs_server.c's vfs_pread(), which has its own
 * notion of "how many bytes did I get", not "give me a C string". Returns
 * the number of bytes actually read (0 at or past EOF), or -1 on a bad
 * argument or a device read error. */
int fat32_read_at(fat32_fs_t *fs, fat32_dir_entry_t *entry, void *buf, uint32_t count, uint64_t offset,
                  fat32_cursor_t *cur) {
    if (!fs || !entry || !buf || !fs->dev || !fs->dev->read_blocks) return -1;
    uint64_t file_size = entry->file_size;
    if (offset >= file_size || count == 0) return 0;

    uint64_t remaining_in_file = file_size - offset;
    uint32_t to_read = (remaining_in_file < count) ? (uint32_t)remaining_in_file : count;

    uint32_t first = ((uint32_t)entry->fst_clus_hi << 16) | entry->fst_clus_lo;
    uint32_t bpc = fs->bytes_per_cluster ? fs->bytes_per_cluster : 512;
    if (first < 2) return 0;
    /* Without a caller's cursor, still never re-walk within this call. */
    fat32_cursor_t local = {0};
    if (!cur) cur = &local;

    ylock_acquire(&fs->lock);
    uint32_t index = (uint32_t)(offset / bpc);
    uint32_t cluster = chain_seek(fs, first, index, cur, false);
    if (cluster == 0) {
        ylock_release(&fs->lock);
        return 0;
    }

    uint32_t skip = (uint32_t)(offset % bpc);
    uint32_t sector_in_cluster = skip / 512;
    uint32_t offset_in_sector = skip % 512;
    uint32_t sec_per_clus = fs->bpb.sec_per_clus ? fs->bpb.sec_per_clus : 1;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t read_total = 0;

    while (read_total < to_read) {
        while (sector_in_cluster < sec_per_clus && read_total < to_read) {
            uint32_t lba = cluster_to_lba(fs, cluster) + sector_in_cluster;
            uint32_t chunk = 512 - offset_in_sector;
            if (chunk > to_read - read_total) chunk = to_read - read_total;
            int rc;
            if (chunk == 512) {
                /* A whole sector: straight into the caller's buffer. */
                rc = fs->dev->read_blocks(fs->dev, dst + read_total, lba, 1);
            } else {
                uint8_t sec[512];
                rc = fs->dev->read_blocks(fs->dev, sec, lba, 1);
                if (rc == 0) memcpy(dst + read_total, sec + offset_in_sector, chunk);
            }
            if (rc != 0) {
                /* What a failed read left in the buffer is not the file. */
                ylock_release(&fs->lock);
                printk("[FAT32] Device '%s': read error at lba %lu\n", fs->dev->name ? fs->dev->name : "unknown", (unsigned long)lba);
                return -1;
            }
            read_total += chunk;
            sector_in_cluster++;
            offset_in_sector = 0;
        }
        if (read_total < to_read) {
            cluster = chain_seek(fs, first, ++index, cur, false);
            if (cluster == 0) break;
            sector_in_cluster = 0;
        }
    }
    ylock_release(&fs->lock);
    return (int)read_total;
}

int fat32_read_file(fat32_fs_t *fs, fat32_dir_entry_t *entry, void *buf, uint32_t max_size) {
    if (!fs || !entry || !buf) return -1;
    if (max_size == 0) return 0;
    /* Reserve the last byte of the caller's buffer for the NUL terminator
     * written below, regardless of what max_size the caller passed. Most
     * callers already pass sizeof(buf) - 1 themselves, but at least one
     * (arch/riscv/common/elf.c) passed sizeof(file_buf) directly, which
     * wrote one byte past the end of its buffer whenever the file being
     * read was >= that size (see B7 in
     * plan/completed/2026-08-07_review_and_remediation.md). Clamping here makes the
     * function safe regardless of what any given caller passes. */
    int n = fat32_read_at(fs, entry, buf, max_size - 1, 0, NULL);
    if (n < 0) return -1;
    ((char *)buf)[n] = '\0';
    return n;
}

/* --- fat32_write_file --- */

static int fat32_write_file_nolock(fat32_fs_t *fs, const char *path, const void *buf, uint32_t size) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    const char *dn = fs->dev->name ? fs->dev->name : "unknown";
    size_t plen = strlen(path);
    if (plen > 0 && path[plen - 1] == '/') {
        printk("[FAT32] Device '%s': '%s' names a directory, not a file\n", dn, path);
        return -1;
    }
    const char *name;
    size_t len;
    uint32_t parent = path_parent(fs, path, &name, &len);
    if (parent == 0 || len == 0) {
        /* Each failure names itself: the shell's `write` can only say `#f`,
         * and a full directory was once investigated as a space problem
         * because nothing here said otherwise. */
        printk("[FAT32] Device '%s': no directory to create '%s' in\n", dn, path);
        return -1;
    }
    /* One pass finds the file, or where a new one goes and its alias. */
    dir_find_t f;
    char cand[ALIAS_MAX][11];
    int32_t units = create_query(&f, name, len, 0, cand);
    if (f.units < 0) {
        printk("[FAT32] Device '%s': '%s' is not a valid file name\n", dn, path);
        return -1;
    }
    dir_find(fs, parent, &f);
    if (f.io_error) return -1;
    if (!f.found && units < 0) {
        printk("[FAT32] Device '%s': '%s' is not a valid file name\n", dn, path);
        return -1;
    }
    if (f.found && (f.entry.attr & FAT32_ATTR_DIRECTORY)) {
        printk("[FAT32] Device '%s': '%s' is a directory\n", dn, path);
        return -1;
    }

    /* Overwriting: the old chain is freed first, so it does not leak. */
    if (f.found) {
        uint32_t old_clus = entry_first_cluster(&f.entry);
        if (old_clus) fat32_free_chain(fs, old_clus);
    }

    /* One cluster per bytes_per_cluster, every sector of it written. An empty
     * file has no cluster at all (first cluster 0), as the FAT spec has it;
     * fsck.fat truncates anything else. fat32_write_at() gives it one on its
     * first write. */
    uint32_t bpc = fs->bytes_per_cluster ? fs->bytes_per_cluster : 512;
    uint32_t sec_per_clus = fs->bpb.sec_per_clus ? fs->bpb.sec_per_clus : 1;
    uint32_t clusters_needed = (size + bpc - 1) / bpc;
    uint32_t first_cluster = 0, cur_clus = 0;
    bool io_error = false;

    for (uint32_t c = 0; c < clusters_needed; c++) {
        uint32_t next_clus = fat_alloc_cluster(fs);
        if (next_clus == 0) {
            printk("[FAT32] Device '%s': volume full writing '%s' (%u of %u clusters placed)\n",
                   dn, path, (unsigned int)c, (unsigned int)clusters_needed);
            /* The clusters placed so far are freed again. An entry being
             * overwritten already lost its old chain, so it is left empty
             * rather than pointing at freed clusters. */
            if (first_cluster) fat32_free_chain(fs, first_cluster);
            if (f.found) {
                fat32_dir_entry_t e = f.entry;
                e.fst_clus_hi = e.fst_clus_lo = 0;
                e.file_size = 0;
                (void)slot_write(fs, f.at, &e);
            }
            return -1;
        }
        if (c == 0) first_cluster = next_clus;
        else fat_set_entry(fs, cur_clus, next_clus);
        cur_clus = next_clus;

        uint32_t data_lba = cluster_to_lba(fs, cur_clus);
        for (uint32_t sct = 0; sct < sec_per_clus; sct++) {
            uint32_t offset = c * bpc + sct * 512;
            if (offset >= size && sct > 0) break;   /* past EOF: never read */
            uint32_t chunk = (size > offset) ? (size - offset) : 0;
            if (chunk > 512) chunk = 512;
            if (buf && chunk == 512) {
                if (fs->dev->write_blocks(fs->dev, (const uint8_t *)buf + offset, data_lba + sct, 1) != 0)
                    io_error = true;
                continue;
            }
            uint8_t data_sec[512];
            memset(data_sec, 0, 512);
            if (buf && chunk > 0) memcpy(data_sec, (const uint8_t *)buf + offset, chunk);
            if (fs->dev->write_blocks(fs->dev, data_sec, data_lba + sct, 1) != 0) io_error = true;
        }
    }

    /* The entry is recorded even after a failed data write, so the chain is
     * not leaked; the caller is told the content is not what it wrote. */
    fat32_dir_entry_t e;
    int rc;
    if (f.found) {
        e = f.entry;
        e.fst_clus_hi = (uint16_t)(first_cluster >> 16);
        e.fst_clus_lo = (uint16_t)(first_cluster & 0xFFFF);
        e.file_size = size;
        rc = slot_write(fs, f.at, &e);
    } else {
        memset(&e, 0, sizeof e);
        e.attr = FAT32_ATTR_ARCHIVE;
        e.fst_clus_hi = (uint16_t)(first_cluster >> 16);
        e.fst_clus_lo = (uint16_t)(first_cluster & 0xFFFF);
        e.file_size = size;
        rc = dir_create(fs, parent, name, len, &e, NULL, NULL, &f);
    }
    if (rc != 0) {
        if (first_cluster) fat32_free_chain(fs, first_cluster);
        printk("[FAT32] Device '%s': could not record '%s' in its directory\n", dn, path);
        return -1;
    }
    if (io_error) {
        printk("[FAT32] Device '%s': write error in '%s'\n", dn, path);
        return -1;
    }
    return 0;
}

int fat32_write_file(fat32_fs_t *fs, const char *path, const void *buf, uint32_t size) {
    if (!fs) return -1;
    ylock_acquire(&fs->lock);
    int r = fat32_write_file_nolock(fs, path, buf, size);
    ylock_release(&fs->lock);
    return r;
}

/* Writes `count` bytes at byte `offset` within the file whose 8.3 entry is
 * at *loc: walks (or extends) the cluster chain to `offset`, read-modify-
 * writes partial sectors, and allocates fresh clusters for anything past the
 * chain, so a write past EOF zero-fills the gap. Never truncates: file_size
 * only grows here.
 *
 * *entry is the caller's copy of the entry. The slot at *loc must still hold
 * an entry of that 8.3 name -- a file deleted or renamed since it was opened
 * is not written into whatever took its slot -- and *entry is refreshed from
 * it (size and first cluster as this write left them). Returns bytes
 * written, or -1. */
int fat32_write_at_loc(fat32_fs_t *fs, const fat32_loc_t *loc, fat32_dir_entry_t *entry,
                       const void *buf, uint32_t count, uint64_t offset, fat32_cursor_t *cur) {
    if (!fs || !loc || !entry || !buf || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    if (count == 0) return 0;
    fat32_cursor_t local = {0};
    if (!cur) cur = &local;

    ylock_acquire(&fs->lock);
    fat32_dir_entry_t now;
    if (loc->clus < 2 || slot_read(fs, *loc, &now) != 0 || dir_entry_is_free(&now) || slot_is_lfn(&now) ||
        memcmp(now.name, entry->name, 11) != 0 || (now.attr & FAT32_ATTR_DIRECTORY)) {
        ylock_release(&fs->lock);
        printk("[FAT32] Device '%s': a file open for writing was removed or renamed\n",
               fs->dev->name ? fs->dev->name : "unknown");
        return -1;
    }
    uint32_t first_cluster = entry_first_cluster(&now);
    uint32_t bpc = fs->bytes_per_cluster ? fs->bytes_per_cluster : 512;
    bool new_first = false;
    if (first_cluster == 0) {
        /* An empty file's first write gives it its first cluster. Zeroed
         * only when the write starts past it, since then part of it is a gap
         * that reads back as zeros. */
        first_cluster = fat_alloc_cluster(fs);
        if (first_cluster == 0) {
            ylock_release(&fs->lock);
            return -1;
        }
        if (offset > 0) {
            uint8_t zero_sec[512];
            memset(zero_sec, 0, 512);
            for (uint32_t z = 0; z < fs->bpb.sec_per_clus; z++) {
                fs->dev->write_blocks(fs->dev, zero_sec, cluster_to_lba(fs, first_cluster) + z, 1);
            }
        }
        new_first = true;
        now.fst_clus_hi = (uint16_t)(first_cluster >> 16);
        now.fst_clus_lo = (uint16_t)(first_cluster & 0xFFFF);
    }

    /* Walk (or extend, zero-filled) to the cluster containing `offset`. */
    uint32_t index = (uint32_t)(offset / bpc);
    uint32_t cluster = chain_seek(fs, first_cluster, index, cur, true);
    if (cluster == 0) {
        /* Out of space on the way to `offset`. A first cluster given to an
         * empty file just now is recorded anyway, so nothing leaks. */
        if (new_first) (void)slot_write(fs, *loc, &now);
        *entry = now;
        ylock_release(&fs->lock);
        return -1;
    }

    uint32_t offset_in_cluster = (uint32_t)(offset % bpc);
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t written = 0;
    bool io_error = false;

    while (written < count && !io_error) {
        while (offset_in_cluster < bpc && written < count) {
            uint32_t sector_in_cluster = offset_in_cluster / 512;
            uint32_t offset_in_sector = offset_in_cluster % 512;
            uint32_t lba = cluster_to_lba(fs, cluster) + sector_in_cluster;

            uint32_t space = 512 - offset_in_sector;
            uint32_t chunk = (count - written) < space ? (count - written) : space;
            if (chunk == 512) {
                /* A whole sector replaces what was there: no read needed. */
                if (fs->dev->write_blocks(fs->dev, src + written, lba, 1) != 0) { io_error = true; break; }
            } else {
                uint8_t sec[512];
                if (fs->dev->read_blocks(fs->dev, sec, lba, 1) != 0) { io_error = true; break; }
                memcpy(sec + offset_in_sector, src + written, chunk);
                if (fs->dev->write_blocks(fs->dev, sec, lba, 1) != 0) { io_error = true; break; }
            }
            written += chunk;
            offset_in_cluster += chunk;
        }
        if (written < count && !io_error) {
            /* Extending here does not zero the new cluster first: every byte
             * up to the new EOF is about to be written, and what lies past
             * EOF is never read. */
            uint32_t next = fat_get_entry(fs, cluster);
            if (next >= 0x0FFFFFF8) {
                uint32_t new_clus = fat_alloc_cluster(fs);
                if (new_clus == 0) break;   /* out of space; keep whatever was written */
                fat_set_entry(fs, cluster, new_clus);
                next = new_clus;
            } else if (next < 2) {
                break;   /* damaged chain */
            }
            cluster = next;
            index++;
            cur->first = first_cluster;
            cur->gen = fs->gen;
            cur->index = index;
            cur->cluster = cluster;
            offset_in_cluster = 0;
        }
    }

    uint64_t new_size = offset + written;
    if (new_size > now.file_size || new_first) {
        if (new_size > now.file_size) now.file_size = (uint32_t)new_size;
        (void)slot_write(fs, *loc, &now);
    }
    *entry = now;
    ylock_release(&fs->lock);
    if (io_error) {
        printk("[FAT32] Device '%s': write error at %lu\n", fs->dev->name ? fs->dev->name : "unknown",
               (unsigned long)(offset + written));
        return -1;
    }
    return (int)written;
}

/* fat32_write_at_loc() by path. A file that does not exist is created when
 * the write starts at offset 0. */
int fat32_write_at(fat32_fs_t *fs, const char *path, const void *buf, uint32_t count, uint64_t offset,
                   fat32_cursor_t *cur) {
    if (!fs || !path || !buf) return -1;
    if (count == 0) return 0;
    ylock_acquire(&fs->lock);
    fat32_dir_entry_t entry;
    fat32_loc_t loc;
    int r;
    if (fat32_lookup_nolock(fs, path, &entry, &loc) != 0) {
        r = (offset == 0 && fat32_write_file_nolock(fs, path, buf, count) == 0) ? (int)count : -1;
    } else {
        r = fat32_write_at_loc(fs, &loc, &entry, buf, count, offset, cur);
    }
    ylock_release(&fs->lock);
    return r;
}

/* Frees an existing file's cluster chain and resets it to empty (size 0, no
 * first cluster) without deleting its directory entry -- the primitive
 * behind VFS_O_TRUNC. fat32_write_at() allocates the first cluster on the
 * first write. */
int fat32_truncate(fat32_fs_t *fs, const char *path) {
    return fat32_write_file(fs, path, NULL, 0);
}

/* Appends `len` bytes to the end of a file, creating it if it is missing:
 * O(1) per call, where rewriting the whole file was O(file). */
int fat32_append_file(fat32_fs_t *fs, const char *path, const void *buf, uint32_t len) {
    if (!fs || !path || !buf || len == 0) return 0;
    ylock_acquire(&fs->lock);
    int r;
    fat32_dir_entry_t entry;
    fat32_loc_t loc;
    if (fat32_lookup_nolock(fs, path, &entry, &loc) != 0) {
        r = (fat32_write_file_nolock(fs, path, buf, len) == 0) ? (int)len : -1;
    } else {
        r = fat32_write_at_loc(fs, &loc, &entry, buf, len, entry.file_size, NULL);
    }
    ylock_release(&fs->lock);
    return r;
}

/* --- fat32_mkdir --- */

static int fat32_mkdir_nolock(fat32_fs_t *fs, const char *path) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    const char *name;
    size_t len;
    uint32_t parent_clus = path_parent(fs, path, &name, &len);
    if (parent_clus == 0 || len == 0 || name_units(name, len) < 0) return -1;

    /* Checked before a cluster is spent on it: `mkdir` of an existing
     * directory is routine (the shell's history directory, at every boot). */
    dir_find_t f;
    char cand[ALIAS_MAX][11];
    (void)create_query(&f, name, len, 0, cand);
    dir_find(fs, parent_clus, &f);
    if (f.found || f.io_error) return -1;

    uint32_t new_clus = fat_alloc_cluster(fs);
    if (new_clus == 0) {
        printk("[FAT32] Device '%s': no free cluster for the new directory '%s'\n",
               fs->dev->name ? fs->dev->name : "unknown", path);
        return -1;
    }

    fat32_sector_t dir_sec;
    memset(&dir_sec, 0, sizeof(dir_sec));
    fat32_dir_entry_t *dot = &dir_sec.entries[0];
    memcpy(dot->name, ".          ", 11);
    dot->attr = FAT32_ATTR_DIRECTORY;
    dot->fst_clus_hi = (uint16_t)(new_clus >> 16);
    dot->fst_clus_lo = (uint16_t)(new_clus & 0xFFFF);
    fat32_dir_entry_t *dotdot = &dir_sec.entries[1];
    memcpy(dotdot->name, "..         ", 11);
    dotdot->attr = FAT32_ATTR_DIRECTORY;
    uint32_t up_clus = (parent_clus == fs->root_dir_cluster) ? 0 : parent_clus;
    dotdot->fst_clus_hi = (uint16_t)(up_clus >> 16);
    dotdot->fst_clus_lo = (uint16_t)(up_clus & 0xFFFF);

    uint32_t base_lba = cluster_to_lba(fs, new_clus);
    if (fs->dev->write_blocks(fs->dev, dir_sec.raw, base_lba, 1) != 0) {
        fat32_free_chain(fs, new_clus);
        return -1;
    }
    /* The rest of the cluster zeroed, so stale card data is not read as entries. */
    memset(&dir_sec, 0, sizeof(dir_sec));
    for (uint32_t sec = 1; sec < fs->bpb.sec_per_clus; sec++) {
        if (fs->dev->write_blocks(fs->dev, dir_sec.raw, base_lba + sec, 1) != 0) {
            fat32_free_chain(fs, new_clus);
            return -1;
        }
    }

    fat32_dir_entry_t e;
    memset(&e, 0, sizeof e);
    e.attr = FAT32_ATTR_DIRECTORY;
    e.fst_clus_hi = (uint16_t)(new_clus >> 16);
    e.fst_clus_lo = (uint16_t)(new_clus & 0xFFFF);
    if (dir_create(fs, parent_clus, name, len, &e, NULL, NULL, &f) != 0) {
        fat32_free_chain(fs, new_clus);
        return -1;
    }
    /* No line on success: on a board whose 9P shares the console's wire
     * (`p9share`), a log line in the middle of a session lands between a
     * reply's bytes and breaks its framing. */
    return 0;
}

int fat32_mkdir(fat32_fs_t *fs, const char *path) {
    if (!fs) return -1;
    ylock_acquire(&fs->lock);
    int r = fat32_mkdir_nolock(fs, path);
    ylock_release(&fs->lock);
    return r;
}

/* --- fat32_remove_file --- */

/* Removes the entry `path` names, which must be a directory if `want_dir`
 * and must not be one otherwise: a directory removed as a file would orphan
 * its children's clusters for good. Only fat32_rmdir(), after checking the
 * directory is empty, removes one. */
static int fat32_remove_entry_nolock(fat32_fs_t *fs, const char *path, bool want_dir) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    dir_find_t f;
    if (lookup(fs, path, &f, NULL) != 0) return -1;
    bool is_dir = (f.entry.attr & FAT32_ATTR_DIRECTORY) != 0;
    if (is_dir != want_dir) {
        if (!want_dir) cprintf("rm: '%s' is a directory; use rmdir\n", path);
        return -1;
    }
    uint32_t clus = entry_first_cluster(&f.entry);
    if (clus) fat32_free_chain(fs, clus);
    return dir_erase_set(fs, f.set, f.set_len);
}

int fat32_remove_file(fat32_fs_t *fs, const char *path) {
    if (!fs) return -1;
    ylock_acquire(&fs->lock);
    int r = fat32_remove_entry_nolock(fs, path, false);
    ylock_release(&fs->lock);
    return r;
}

/* --- fat32_rmdir: empty-check scan --- */

typedef struct {
    bool has_real_entries;
} empty_check_ctx_t;

static bool dir_empty_check_cb(fat32_fs_t *fs, uint32_t sector_lba,
                                fat32_dir_entry_t *entries, int count, void *vctx) {
    (void)fs; (void)sector_lba;
    empty_check_ctx_t *ctx = (empty_check_ctx_t *)vctx;
    for (int i = 0; i < count; i++) {
        if (entries[i].name[0] == 0x00) return true;
        if ((uint8_t)entries[i].name[0] == 0xE5) continue;
        if (slot_is_lfn(&entries[i])) continue;          /* an orphaned long name holds no file */
        if (dir_entry_is_corrupt(&entries[i])) continue;
        if (memcmp(entries[i].name, ".          ", 11) == 0 ||
            memcmp(entries[i].name, "..         ", 11) == 0) {
            continue;
        }
        ctx->has_real_entries = true;
        return true;
    }
    return false;
}

static int fat32_rmdir_nolock(fat32_fs_t *fs, const char *path) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    fat32_dir_entry_t entry;
    if (fat32_lookup_nolock(fs, path, &entry, NULL) < 0) return -1;

    if (!(entry.attr & FAT32_ATTR_DIRECTORY)) {
        cprintf("rmdir: '%s' is not a directory\n", path);
        return -1;
    }
    uint32_t dir_clus = entry_first_cluster(&entry);
    if (dir_clus == 0 || dir_clus == fs->root_dir_cluster) {
        cprintf("rmdir: cannot remove root directory\n");
        return -1;
    }
    empty_check_ctx_t ctx = { .has_real_entries = false };
    fat32_scan_dir(fs, dir_clus, dir_empty_check_cb, &ctx);
    if (ctx.has_real_entries) {
        cprintf("rmdir: directory '%s' is not empty\n", path);
        return -1;
    }
    /* fat32_remove_entry_nolock() frees the directory's chain with its set. */
    return fat32_remove_entry_nolock(fs, path, true);
}

int fat32_rmdir(fat32_fs_t *fs, const char *path) {
    if (!fs) return -1;
    ylock_acquire(&fs->lock);
    int r = fat32_rmdir_nolock(fs, path);
    ylock_release(&fs->lock);
    return r;
}

/* --- fat32_rename --- */

/* Whether directory `clus` is `dir` or lies somewhere below it, by its ".."
 * entries up to the root. */
static bool dir_is_within(fat32_fs_t *fs, uint32_t clus, uint32_t dir) {
    for (uint32_t guard = 0; guard < 4096u; guard++) {
        if (clus == dir) return true;
        if (clus == fs->root_dir_cluster || clus < 2) return false;
        fat32_dir_entry_t up;
        fat32_loc_t at = { clus, 1 };
        if (slot_read(fs, at, &up) != 0 || memcmp(up.name, "..         ", 11) != 0) return false;
        uint32_t c = entry_first_cluster(&up);
        clus = c ? c : fs->root_dir_cluster;
    }
    return true;   /* a ".." cycle: refuse rather than guess */
}

static int fat32_rename_nolock(fat32_fs_t *fs, const char *from, const char *to) {
    if (!fs || !from || !to || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    dir_find_t src;
    uint32_t src_parent;
    if (lookup(fs, from, &src, &src_parent) != 0) return -1;
    const char *name;
    size_t len;
    uint32_t dst_parent = path_parent(fs, to, &name, &len);
    if (dst_parent == 0 || len == 0 || name_units(name, len) < 0) return -1;
    if (dst_parent == src_parent && len == src.len && memcmp(name, src.name, len) == 0) return 0;

    bool is_dir = (src.entry.attr & FAT32_ATTR_DIRECTORY) != 0;
    uint32_t dclus = entry_first_cluster(&src.entry);
    if (is_dir && dst_parent != src_parent && dclus && dir_is_within(fs, dst_parent, dclus)) {
        cprintf("mv: cannot move '%s' into itself\n", from);
        return -1;
    }

    /* The new set first, then the old one erased: a power cut in between
     * leaves the file under both names (fsck reports the shared chain), never
     * under neither. Within one directory the source does not count as "the
     * new name exists", so a change of case alone is a rename too. */
    int rc = dir_create(fs, dst_parent, name, len, &src.entry,
                        dst_parent == src_parent ? &src.at : NULL, NULL, NULL);
    if (rc == -2) {
        cprintf("mv: '%s' already exists\n", to);
        return -1;
    }
    if (rc != 0) return -1;
    if (dir_erase_set(fs, src.set, src.set_len) != 0) return -1;

    /* A directory that changed parents points its ".." at the new one. */
    if (is_dir && dst_parent != src_parent && dclus) {
        fat32_dir_entry_t up;
        fat32_loc_t at = { dclus, 1 };
        if (slot_read(fs, at, &up) == 0 && memcmp(up.name, "..         ", 11) == 0) {
            uint32_t c = dst_parent == fs->root_dir_cluster ? 0 : dst_parent;
            up.fst_clus_hi = (uint16_t)(c >> 16);
            up.fst_clus_lo = (uint16_t)(c & 0xFFFF);
            if (slot_write(fs, at, &up) != 0) return -1;
        }
    }
    return 0;
}

int fat32_rename(fat32_fs_t *fs, const char *from, const char *to) {
    if (!fs) return -1;
    ylock_acquire(&fs->lock);
    int r = fat32_rename_nolock(fs, from, to);
    ylock_release(&fs->lock);
    return r;
}

int fat32_statfs(fat32_fs_t *fs, uint32_t *total_blocks, uint32_t *free_blocks) {
    if (!fs || !fs->dev) return -1;
    ylock_acquire(&fs->lock);
    uint32_t total_sec = fs->bpb.tot_sec32 ? fs->bpb.tot_sec32 : fs->bpb.tot_sec16;
    /* In 512-byte blocks, not bytes: a byte count overflows 32 bits past
     * 4 GiB, and a 128 GB card read as 3.3 GB in `df` (2026-09-30). */
    uint32_t blocks_per_sec = fs->bpb.bytes_per_sec / 512u;
    if (total_blocks) *total_blocks = total_sec * blocks_per_sec;

    /* The full scan is now the fallback, run at most once per mount: a
     * 128 GB card's FAT is ~15 MB, and reading all of it over SPI is what
     * made `df` take 42 s (plan/open_issues.md, 2026-09-30). It also used to
     * count from entry 0 -- the two reserved entries -- and so missed the
     * volume's last two clusters. */
    if (fs->free_count == FAT32_FREE_UNKNOWN) {
        uint32_t free_clusters = 0;
        uint32_t end = fs->total_clusters + 2;   /* entries 2 .. end-1 */
        uint32_t fat_buf[128];
        bool ok = true;
        for (uint32_t first = 0; first < end; first += 128) {
            if (fs->dev->read_blocks(fs->dev, fat_buf, fs->fat_start_sector + first / 128, 1) != 0) {
                ok = false;
                break;
            }
            for (uint32_t i = 0; i < 128 && first + i < end; i++) {
                if (first + i >= 2 && (fat_buf[i] & 0x0FFFFFFF) == 0) free_clusters++;
            }
        }
        if (ok) fs->free_count = free_clusters;
    }
    uint32_t free_clusters = (fs->free_count == FAT32_FREE_UNKNOWN) ? 0 : fs->free_count;
    if (free_blocks) *free_blocks = free_clusters * (fs->bytes_per_cluster / 512u);
    fat32_sync(fs);
    ylock_release(&fs->lock);
    return 0;
}

/* --- fat32_check --- */

typedef struct {
    uint8_t       *bits;
    uint32_t      *q;
    uint32_t       qcap, qhead, qtail;
    fat32_check_t *out;
    uint32_t       printed;
} check_ctx_t;

static void check_say(check_ctx_t *c, const char *what, const char raw[11], uint32_t clus) {
    if (c->printed++ >= 8u) return;
    fat32_dir_entry_t e;
    memset(&e, 0, sizeof e);
    memcpy(e.name, raw, 11);
    char name[16];
    short_display(&e, name, sizeof name);
    printk("[FAT32] check: %s: '%s' at cluster %lu\n", what, name, (unsigned long)clus);
}

/* Marks the chain from `first` and returns how many clusters it has. */
static uint32_t check_chain(fat32_fs_t *fs, check_ctx_t *c, uint32_t first, const char raw[11]) {
    uint32_t n = 0, clus = first;
    while (n <= fs->total_clusters) {
        if (clus < 2 || clus >= fs->total_clusters + 2) {
            c->out->bad_chain++;
            check_say(c, "chain leaves the volume", raw, clus);
            break;
        }
        uint8_t m = (uint8_t)(1u << (clus & 7u));
        if (c->bits[clus >> 3] & m) {
            c->out->cross++;
            check_say(c, "cluster already claimed (cross-linked)", raw, clus);
            break;
        }
        c->bits[clus >> 3] |= m;
        c->out->clusters++;
        n++;
        uint32_t next;
        if (!fat_read_entry(fs, clus, &next)) break;
        if (next >= 0x0FFFFFF8) break;
        if (next < 2) {
            c->out->bad_chain++;
            check_say(c, "chain runs into a free entry", raw, clus);
            break;
        }
        clus = next;
    }
    return n;
}

static bool check_dir_cb(fat32_fs_t *fs, uint32_t sector_lba, fat32_dir_entry_t *entries,
                         int count, void *vctx) {
    (void)sector_lba;
    check_ctx_t *c = (check_ctx_t *)vctx;
    for (int i = 0; i < count; i++) {
        fat32_dir_entry_t *e = &entries[i];
        if (e->name[0] == 0x00) return true;          /* end of directory */
        if (dir_entry_is_skippable(e) || e->name[0] == '.') continue;
        uint32_t first = ((uint32_t)e->fst_clus_hi << 16) | e->fst_clus_lo;
        uint32_t bpc = fs->bytes_per_cluster ? fs->bytes_per_cluster : 512u;
        if (e->attr & FAT32_ATTR_DIRECTORY) {
            c->out->dirs++;
            if (first == 0) continue;
            uint8_t m = (uint8_t)(1u << (first & 7u));
            bool seen = first >= 2 && first < fs->total_clusters + 2 && (c->bits[first >> 3] & m);
            (void)check_chain(fs, c, first, e->name);
            if (seen) continue;                        /* reported; not walked twice */
            if (c->qtail - c->qhead < c->qcap) c->q[c->qtail++ % c->qcap] = first;
            else c->out->skipped++;
        } else {
            c->out->files++;
            uint32_t need = (e->file_size + bpc - 1u) / bpc;
            uint32_t have = first ? check_chain(fs, c, first, e->name) : 0u;
            if (have != need && !(first == 0 && need == 0)) {
                c->out->size++;
                check_say(c, "size does not match its chain", e->name, first);
            }
        }
    }
    return false;
}

int fat32_check(fat32_fs_t *fs, uint8_t *bitmap, uint32_t bitmap_bytes,
                uint32_t *dirq, uint32_t dirq_cap, fat32_check_t *out) {
    if (!fs || !fs->dev || !bitmap || !dirq || dirq_cap == 0 || !out) return -1;
    if (bitmap_bytes < (fs->total_clusters + 2u + 7u) / 8u) return -1;
    memset(out, 0, sizeof *out);
    memset(bitmap, 0, (fs->total_clusters + 2u + 7u) / 8u);
    check_ctx_t c = { .bits = bitmap, .q = dirq, .qcap = dirq_cap, .out = out };
    static const char k_root[11] = { '/', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };

    ylock_acquire(&fs->lock);
    (void)check_chain(fs, &c, fs->root_dir_cluster, k_root);
    c.q[c.qtail++] = fs->root_dir_cluster;
    while (c.qhead != c.qtail) {
        uint32_t d = c.q[c.qhead++ % c.qcap];
        fat32_scan_dir(fs, d, check_dir_cb, &c);
    }
    /* What the FAT holds that nothing reached. */
    int rc = 0;
    uint32_t end = fs->total_clusters + 2u;
    for (uint32_t clus = 2; clus < end; ) {
        fat32_sector_t sec;
        uint32_t fat_sector = fs->fat_start_sector + (clus * 4u) / 512u;
        if (fs->dev->read_blocks(fs->dev, sec.raw, fat_sector, 1) != 0) { rc = -1; break; }
        for (uint32_t k = (clus * 4u % 512u) / 4u; k < 128u && clus < end; k++, clus++) {
            if ((sec.words[k] & 0x0FFFFFFFu) != 0 && !(bitmap[clus >> 3] & (1u << (clus & 7u))))
                out->lost++;
        }
    }
    ylock_release(&fs->lock);
    return rc;
}

uint32_t fat32_recount(fat32_fs_t *fs) {
    if (!fs || !fs->dev) return FAT32_FREE_UNKNOWN;
    ylock_acquire(&fs->lock);
    uint32_t before = fs->free_count;
    fs->free_count = FAT32_FREE_UNKNOWN;
    /* Counted as a change, so fat32_statfs()'s sync writes the result: the
     * point of a recount is usually an FSInfo that someone else let go stale
     * (any FAT writer that did not maintain it -- LugalOS before 38.0). */
    fs->modified = true;
    fs->fsinfo_valid = false;
    fat32_statfs(fs, NULL, NULL);
    ylock_release(&fs->lock);
    return before;
}

void fat32_sync(fat32_fs_t *fs) {
    if (!fs || !fs->dev) return;
    ylock_acquire(&fs->lock);
    /* Only a volume this mount has changed: /flash0 is read-only, and a
     * count we merely computed is no reason to write to anyone's card. */
    if (fs->modified && !fs->fsinfo_valid && fs->free_count != FAT32_FREE_UNKNOWN && fs->fsinfo_lba) {
        fsinfo_write(fs, fs->free_count);
        fs->fsinfo_valid = true;
    }
    ylock_release(&fs->lock);
}

void fat32_set_fat_cache(fat32_fs_t *fs, uint32_t *buf128) {
    if (!fs) return;
    ylock_acquire(&fs->lock);
    fs->fat_cache = buf128;
    fs->fat_cache_lba = 0;
    ylock_release(&fs->lock);
}


/* --- fat32_readdir --- */

static int fat32_readdir_nolock(fat32_fs_t *fs, uint32_t dir_cluster, uint32_t index, fat32_cursor_t *cur,
                                char *name_out, uint32_t name_max, fat32_dir_entry_t *out_entry) {
    if (!fs || !fs->dev || !fs->dev->read_blocks) return -1;
    fat32_loc_t at = { dir_cluster, 0 };
    uint32_t i = 0;
    /* Continue after the entry the cursor last returned, when it is still
     * this directory's and not past `index`: listing a directory of n entries
     * is then one pass, not n. */
    if (cur && cur->first == dir_cluster && cur->gen == fs->gen && cur->cluster >= 2 && cur->index <= index) {
        at.clus = cur->cluster;
        at.idx = cur->slot;
        i = cur->index;
    }
    slot_walk_t w;
    walk_start(&w, fs, at);
    lfn_track_t t;
    memset(&t, 0, sizeof t);
    uint16_t units[LFN_MAX_SLOTS * LFN_UNITS];
    fat32_dir_entry_t *e;
    while ((e = walk_next(&w, &at)) != NULL) {
        if (e->name[0] == 0x00) break;                         /* end of directory */
        if (slot_is_lfn(e)) {
            lfn_feed(&t, e, at, NULL, 0, -1, units);
            continue;
        }
        bool owned = lfn_owns(&t, e);
        t.seq = 0;
        if ((uint8_t)e->name[0] == 0xE5 || dir_entry_is_skippable(e)) continue;
        if (i++ < index) continue;
        if (out_entry) *out_entry = *e;
        if (name_out && name_max > 0 && !(owned && lfn_to_utf8(units, t.units, name_out, name_max))) {
            short_display(e, name_out, name_max);
        }
        if (cur) {
            cur->first = dir_cluster;
            cur->gen = fs->gen;
            cur->index = i;
            cur->cluster = w.clus;
            cur->slot = w.idx;
        }
        return 0;
    }
    return -1;
}

int fat32_readdir(fat32_fs_t *fs, uint32_t dir_cluster, uint32_t index, fat32_cursor_t *cur,
                  char *name_out, uint32_t name_max, fat32_dir_entry_t *out_entry) {
    if (!fs) return -1;
    ylock_acquire(&fs->lock);
    int r = fat32_readdir_nolock(fs, dir_cluster, index, cur, name_out, name_max, out_entry);
    ylock_release(&fs->lock);
    return r;
}

/* --- fat32_list_dir --- */

static void fat32_list_dir_nolock(fat32_fs_t *fs, const char *path) {
    if (!fs || !fs->dev || !fs->dev->read_blocks) return;
    uint32_t target_clus = fs->root_dir_cluster;
    if (path && *path && strcmp(path, "/") != 0) {
        fat32_dir_entry_t entry;
        if (fat32_lookup_nolock(fs, path, &entry, NULL) < 0) {
            cprintf("ls: path '%s' not found\n", path);
            return;
        }
        if (!(entry.attr & FAT32_ATTR_DIRECTORY)) {
            cprintf("ls: '%s' is not a directory\n", path);
            return;
        }
        target_clus = entry_first_cluster(&entry);
        if (target_clus == 0) target_clus = fs->root_dir_cluster;
    }

    cprintf("\nDirectory Listing (FAT32):\n");
    cprintf("Size (Bytes)  Attr   Type    Name\n");
    cprintf("------------  -----  ------  ----\n");
    fat32_cursor_t cur = {0};
    fat32_dir_entry_t e;
    char name[FAT32_NAME_MAX];
    uint32_t i = 0;
    while (fat32_readdir_nolock(fs, target_clus, i, &cur, name, sizeof name, &e) == 0) {
        cprintf("%12u  0x%02x   %s  %s\n", (unsigned int)e.file_size, e.attr,
                (e.attr & FAT32_ATTR_DIRECTORY) ? "<DIR> " : "<FILE>", name);
        i++;
    }
    if (i == 0) cprintf("(empty directory)\n");
    cprintf("\n");
}

void fat32_list_dir(fat32_fs_t *fs, const char *path) {
    if (!fs) return;
    ylock_acquire(&fs->lock);
    fat32_list_dir_nolock(fs, path);
    ylock_release(&fs->lock);
}
