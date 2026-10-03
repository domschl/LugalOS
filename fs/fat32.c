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

/* strncpy() doesn't null-terminate when src is exactly dst_size-1 or more
 * characters long (e.g. a 63+ character path component into a 64-byte
 * caller buffer) -- see B13 in plan/completed/2026-08-07_review_and_remediation.md.
 * Mirrors strncpy_local() in user/lisp/lisp.c and safe_strncpy() in
 * kernel/line_editor.c: dst_size is the *full* destination buffer size, and
 * the result is always terminated within it. */
static void safe_strncpy(char *dst, const char *src, int dst_size) {
    int i = 0;
    while (i < dst_size - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void filename_to_83(const char *src, char *dst) {
    if (!src || !dst) return;
    memset(dst, ' ', 11);
    if (!*src) return;

    /* Find the last '.' (an extension separator), if any. Scanning for the
     * base name and the extension separately -- rather than stopping the
     * base-name loop the moment it sees a '.' -- matters because a base
     * name of 8+ characters (e.g. "host_test.txt") would otherwise exit
     * that loop via the i<8 bound without ever consuming the '.', so the
     * following "if next char is '.'" check would see the *9th* base
     * character instead and silently drop the extension. That collapsed
     * "host_test.txt" and "host_test_dir" to the same bare "HOST_TES" 8.3
     * name, so creating one made the other's own vfs_mkdir()/fat32_mkdir()
     * see a false "already exists" from fat32_find_file(). */
    const char *dot = NULL;
    for (const char *p = src; *p && *p != '/'; p++) {
        if (*p == '.') dot = p;
    }
    if (dot == src) dot = NULL; /* leading dot ("dotfile"): no extension */

    size_t base_len = dot ? (size_t)(dot - src) : strlen(src);
    if (base_len > 8) base_len = 8;
    int i = 0;
    for (size_t k = 0; k < base_len && src[k] != '/'; k++) {
        char c = src[k];
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[i++] = c;
    }

    if (dot) {
        const char *ext = dot + 1;
        int j = 8;
        while (*ext && *ext != '/' && j < 11) {
            char c = *ext++;
            if (c >= 'a' && c <= 'z') c -= 32;
            dst[j++] = c;
        }
    }
}

/* Converts a raw 11-byte 8.3 directory-entry name (space-padded: 8 bytes
 * base + 3 bytes extension) into a normal "NAME.EXT" (or bare "NAME" if
 * there's no extension) NUL-terminated display string -- the inverse of
 * filename_to_83(). Used by fat32_readdir() so a caller (eventually a
 * remote 9P client) gets a usable filename, not raw padded bytes. */
static void fat83_to_display_name(const char raw[11], char *out, uint32_t out_max) {
    if (!out || out_max == 0) return;
    char base[9];
    int bi = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) base[bi++] = raw[i];
    base[bi] = '\0';

    char ext[4];
    int ei = 0;
    for (int i = 8; i < 11 && raw[i] != ' '; i++) ext[ei++] = raw[i];
    ext[ei] = '\0';

    uint32_t n = 0;
    for (int i = 0; base[i] && n < out_max - 1; i++) out[n++] = base[i];
    if (ei > 0) {
        if (n < out_max - 1) out[n++] = '.';
        for (int i = 0; ext[i] && n < out_max - 1; i++) out[n++] = ext[i];
    }
    out[n] = '\0';
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

/* Read a 32-bit FAT entry for the given cluster */
static uint32_t fat_get_entry(fat32_fs_t *fs, uint32_t cluster) {
    if (!fs || !fs->dev || !fs->dev->read_blocks) return 0x0FFFFFFF;
    uint32_t fat_sector = fs->fat_start_sector + (cluster * 4) / 512;
    uint32_t fat_offset = (cluster * 4) % 512;
    fat32_sector_t tmp;
    const uint32_t *w = fat_sector_words(fs, fat_sector, &tmp);
    /* An unreadable FAT sector ends the chain: the old code returned
     * whatever the stack buffer held, which could be followed anywhere. */
    if (!w) return 0x0FFFFFFF;
    return w[fat_offset / 4] & 0x0FFFFFFF;
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
    do {
        if (fat_get_entry(fs, c) == 0x00000000) {
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
static void fat32_scan_dir(fat32_fs_t *fs, uint32_t start_clus, fat32_dir_scan_fn fn, void *ctx) {
    if (!fs || !fs->dev || !fs->dev->read_blocks || !fn) return;
    uint32_t clus_iter = start_clus;
    while (clus_iter < 0x0FFFFFF8) {
        for (uint32_t s = 0; s < fs->bpb.sec_per_clus; s++) {
            fat32_sector_t sector;
            uint32_t lba = cluster_to_lba(fs, clus_iter) + s;
            fs->dev->read_blocks(fs->dev, sector.raw, lba, 1);
            if (fn(fs, lba, sector.entries, 16, ctx)) return;
        }
        clus_iter = fat_get_entry(fs, clus_iter);
    }
}

static bool dir_entry_is_free(const fat32_dir_entry_t *e) {
    return e->name[0] == 0x00 || (uint8_t)e->name[0] == 0xE5;
}

static bool dir_entry_is_skippable(const fat32_dir_entry_t *e) {
    if ((uint8_t)e->name[0] == 0xE5) return true;               // deleted
    if ((e->attr & 0x0F) == 0x0F) return true;                  // VFAT LFN metadata
    if (e->attr & FAT32_ATTR_VOLUME_ID) return true;             // volume label
    return false;
}

/* --- fat32_get_parent_cluster: find a named component's cluster --- */

typedef struct {
    const char *name83;
    uint32_t root_clus;
    uint32_t found_clus; /* 0 = not found (or found but not a directory) */
} find_component_ctx_t;

static bool find_component_cb(fat32_fs_t *fs, uint32_t sector_lba,
                               fat32_dir_entry_t *entries, int count, void *vctx) {
    (void)fs; (void)sector_lba;
    find_component_ctx_t *ctx = (find_component_ctx_t *)vctx;
    for (int i = 0; i < count; i++) {
        if (entries[i].name[0] == 0x00) return true; /* end of directory */
        if (dir_entry_is_skippable(&entries[i])) continue;
        if (memcmp(entries[i].name, ctx->name83, 11) == 0) {
            if (entries[i].attr & FAT32_ATTR_DIRECTORY) {
                uint32_t clus = ((uint32_t)entries[i].fst_clus_hi << 16) | entries[i].fst_clus_lo;
                ctx->found_clus = clus ? clus : ctx->root_clus;
            }
            return true; /* name matched; caller checks found_clus == 0 for "not a directory" */
        }
    }
    return false;
}

/* Extract parent cluster and target file/directory name from a path.
 * out_name is a caller-owned buffer -- every call site declares it as
 * char[64], which a `char *` parameter can't see or enforce, so that size
 * is an implicit contract rather than something checkable here. */
#define FAT32_OUT_NAME_SIZE 64

static uint32_t fat32_get_parent_cluster(fat32_fs_t *fs, const char *path, char *out_name) {
    if (!fs || !path) return 0;
    while (*path == '/') path++;

    /* A path that does not fit is refused, not truncated: a cut-off path
     * names a different file, which an operation would then act on. */
    char path_copy[256];
    if (strlen(path) >= sizeof(path_copy)) return 0;
    safe_strncpy(path_copy, path, sizeof(path_copy));

    /* "system/" names "system" (phase 40, item 4). The trailing separators
     * used to leave an empty final component, so `ls /flash0/system/` said
     * "not found" while `ls /flash0/system` worked. Whether the name must
     * then be a directory is the caller's check (fat32_find_file()). */
    size_t len = strlen(path_copy);
    while (len > 0 && path_copy[len - 1] == '/') path_copy[--len] = '\0';

    uint32_t cur_clus = fs->root_dir_cluster;
    char *curr = path_copy;

    while (*curr) {
        char *slash = strchr(curr, '/');
        if (!slash) {
            if (out_name) safe_strncpy(out_name, curr, FAT32_OUT_NAME_SIZE);
            return cur_clus;
        }

        *slash = '\0';
        if (strlen(curr) > 0) {
            char name83[11];
            filename_to_83(curr, name83);

            find_component_ctx_t ctx = { .name83 = name83, .root_clus = fs->root_dir_cluster, .found_clus = 0 };
            fat32_scan_dir(fs, cur_clus, find_component_cb, &ctx);

            if (ctx.found_clus == 0) return 0; // Component not found
            cur_clus = ctx.found_clus;
        }
        curr = slash + 1;
    }

    if (out_name) out_name[0] = '\0';
    return cur_clus;
}

int fat32_format(block_dev_t *dev) {
    return fat32_format_quiet(dev, false);
}

/* The layout is sized from the volume (38.6, plan/phase38_psram.md). It used
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

int fat32_init(fat32_fs_t *fs, block_dev_t *dev) {
    return fat32_init_quiet(fs, dev, false);
}

int fat32_init_quiet(fat32_fs_t *fs, block_dev_t *dev, bool quiet) {
    if (!fs || !dev || !dev->read_blocks) return -1;
    fs->dev = dev;

    uint8_t sector[512];
    dev->read_blocks(dev, sector, 0, 1);

    uint32_t partition_lba = 0;

    /* Check if LBA 0 is an MBR Master Boot Record with partition table at offset 0x1BE */
    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        uint8_t *part1 = &sector[0x01BE];
        uint8_t part_type = part1[4];
        uint32_t start_lba = (uint32_t)part1[8] | ((uint32_t)part1[9] << 8) |
                             ((uint32_t)part1[10] << 16) | ((uint32_t)part1[11] << 24);

        if ((part_type == 0x0B || part_type == 0x0C || part_type == 0x07 || part_type == 0x0E) && start_lba > 0 && start_lba < 0x0FFFFFFF) {
            partition_lba = start_lba;
            dev->read_blocks(dev, sector, partition_lba, 1);
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


/* --- fat32_find_file --- */

typedef struct {
    const char *name83;
    fat32_dir_entry_t *out_entry;
    bool found;
} find_file_ctx_t;

static bool find_file_cb(fat32_fs_t *fs, uint32_t sector_lba,
                          fat32_dir_entry_t *entries, int count, void *vctx) {
    (void)fs; (void)sector_lba;
    find_file_ctx_t *ctx = (find_file_ctx_t *)vctx;
    for (int i = 0; i < count; i++) {
        if (entries[i].name[0] == 0x00) return true;
        if (dir_entry_is_skippable(&entries[i])) continue;
        if (memcmp(entries[i].name, ctx->name83, 11) == 0) {
            if (ctx->out_entry) *ctx->out_entry = entries[i];
            ctx->found = true;
            return true;
        }
    }
    return false;
}

static int fat32_find_file_nolock(fat32_fs_t *fs, const char *path, fat32_dir_entry_t *out_entry) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks) return -1;
    while (*path == '/') path++;
    if (*path == '\0') {
        if (out_entry) {
            memset(out_entry, 0, sizeof(fat32_dir_entry_t));
            out_entry->attr = FAT32_ATTR_DIRECTORY;
            out_entry->fst_clus_hi = (uint16_t)(fs->root_dir_cluster >> 16);
            out_entry->fst_clus_lo = (uint16_t)(fs->root_dir_cluster & 0xFFFF);
        }
        return 0;
    }

    char target_name[64];
    uint32_t parent_clus = fat32_get_parent_cluster(fs, path, target_name);
    if (parent_clus == 0 || target_name[0] == '\0') return -1;

    char name83[11];
    filename_to_83(target_name, name83);

    fat32_dir_entry_t entry;
    find_file_ctx_t ctx = { .name83 = name83, .out_entry = &entry, .found = false };
    fat32_scan_dir(fs, parent_clus, find_file_cb, &ctx);
    if (!ctx.found) return -1;

    /* "file.txt/" is not file.txt: a trailing separator asks for a directory. */
    size_t len = strlen(path);
    if (path[len - 1] == '/' && !(entry.attr & FAT32_ATTR_DIRECTORY)) return -1;

    if (out_entry) *out_entry = entry;
    return 0;
}

int fat32_find_file(fat32_fs_t *fs, const char *path, fat32_dir_entry_t *out_entry) {
    if (!fs) return -1;
    ylock_acquire(&fs->lock);
    int r = fat32_find_file_nolock(fs, path, out_entry);
    ylock_release(&fs->lock);
    return r;
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
 * argument. */
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
            if (chunk == 512) {
                /* A whole sector: straight into the caller's buffer. */
                fs->dev->read_blocks(fs->dev, dst + read_total, lba, 1);
            } else {
                uint8_t sec[512];
                fs->dev->read_blocks(fs->dev, sec, lba, 1);
                memcpy(dst + read_total, sec + offset_in_sector, chunk);
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

/* --- fat32_write_file: free-slot search --- */

typedef struct {
    const char *name83;
    int free_sector_lba;   /* -1 = none found yet */
    int free_slot;         /* index within free_sector_lba's sector */
    bool name_matched;     /* true if an existing entry with this name was found */
    fat32_dir_entry_t existing; /* valid iff name_matched */
} write_slot_ctx_t;

static bool write_slot_cb(fat32_fs_t *fs, uint32_t sector_lba,
                           fat32_dir_entry_t *entries, int count, void *vctx) {
    (void)fs;
    write_slot_ctx_t *ctx = (write_slot_ctx_t *)vctx;
    for (int i = 0; i < count; i++) {
        if (memcmp(entries[i].name, ctx->name83, 11) == 0 && !dir_entry_is_free(&entries[i])) {
            ctx->name_matched = true;
            ctx->existing = entries[i];
            ctx->free_sector_lba = (int)sector_lba;
            ctx->free_slot = i;
            return true; // Overwrite this exact entry
        }
        if (dir_entry_is_free(&entries[i]) && ctx->free_sector_lba < 0) {
            ctx->free_sector_lba = (int)sector_lba;
            ctx->free_slot = i;
        }
        if (entries[i].name[0] == 0x00) return true; // End of directory
    }
    return false;
}

/* Claims a directory slot for a new entry, growing the directory if every
 * slot it already has is taken.
 *
 * Until this existed, both write paths below simply gave up there
 * (`if (ctx.free_sector_lba < 0) return -1; // Directory full`), and
 * fat32_scan_dir() only ever walks the chain a directory already has -- so a
 * directory could never hold more entries than its initial allocation, on any
 * device, for the life of the filesystem. What that looks like from the shell
 * is `write /flash0/newname.txt something` returning `#f` on a volume that is
 * 15 % full, while rewriting an existing file works perfectly: the rewrite
 * matches a name and never needs a free slot. Reported against the ESP32-P4's
 * /flash0 (plan/open_issues.md, 2026-09-17) and reproduced immediately on
 * QEMU's /sd0, where the seventh new file in the root failed.
 *
 * Growing a directory is the ordinary FAT32 answer: append a cluster to its
 * chain and zero it. Zeroing is not optional -- a 0x00 first byte is what
 * marks the end of a directory, so an un-zeroed cluster would be read as
 * entries made of whatever that cluster held before.
 *
 * Returns 0 with ctx->free_sector_lba/free_slot set, or -1 having said why. */
#define FAT32_MAX_DIR_CLUSTERS 65536u   /* a cycle in a corrupt FAT must not spin here */

static int dir_claim_slot(fat32_fs_t *fs, uint32_t dir_clus, write_slot_ctx_t *ctx) {
    if (ctx->free_sector_lba >= 0) return 0;   /* the scan already found one */

    /* Walk to the chain's last cluster. Only an end-of-chain marker counts as
     * the end: an entry reading 0 or 1 is not a short chain, it is a damaged
     * one, and linking onto it would be worse than refusing. It would also be
     * unsafe -- a cluster whose FAT entry is 0 is exactly what
     * fat_alloc_cluster() considers free, so it could hand back `last` itself
     * and fat_set_entry() would then point that cluster at itself. */
    uint32_t last = dir_clus;
    bool found_end = false;
    for (uint32_t guard = 0; guard < FAT32_MAX_DIR_CLUSTERS; guard++) {
        uint32_t next = fat_get_entry(fs, last);
        if (next >= 0x0FFFFFF8) { found_end = true; break; }
        if (next < 2) break;   /* damaged: not an end, and not followable */
        last = next;
    }
    if (!found_end) {
        printk("[FAT32] Device '%s': directory cluster chain is damaged or "
               "does not end; refusing to extend it\n",
               fs->dev->name ? fs->dev->name : "unknown");
        return -1;
    }

    uint32_t clus = fat_alloc_cluster(fs);
    if (clus == last) {
        printk("[FAT32] Device '%s': the allocator returned the directory's "
               "own last cluster; refusing to extend it\n",
               fs->dev->name ? fs->dev->name : "unknown");
        return -1;
    }
    if (clus == 0) {
        printk("[FAT32] Device '%s': directory is full and the volume has no "
               "free cluster to extend it with\n",
               fs->dev->name ? fs->dev->name : "unknown");
        return -1;
    }

    fat32_sector_t zero;
    memset(&zero, 0, sizeof(zero));
    uint32_t base = cluster_to_lba(fs, clus);
    for (uint32_t sec = 0; sec < fs->bpb.sec_per_clus; sec++) {
        fs->dev->write_blocks(fs->dev, zero.raw, base + sec, 1);
    }
    fat_set_entry(fs, last, clus);

    ctx->free_sector_lba = (int)base;
    ctx->free_slot = 0;
    return 0;
}

/* Finds `path`'s directory entry via a fresh scan and patches its file_size
 * field in place -- and its first cluster, when `set_first` is non-zero (an
 * empty file has none until its first write, 38.0). Shared by
 * fat32_write_at() and fat32_append_file() (via fat32_write_at()). */
static int fat32_update_entry(fat32_fs_t *fs, const char *path, uint32_t new_size, uint32_t set_first) {
    char target_name[64];
    uint32_t parent_clus = fat32_get_parent_cluster(fs, path, target_name);
    if (parent_clus == 0 || target_name[0] == '\0') return -1;

    char name83[11];
    filename_to_83(target_name, name83);

    write_slot_ctx_t ctx = { .name83 = name83, .free_sector_lba = -1, .free_slot = 0, .name_matched = false };
    fat32_scan_dir(fs, parent_clus, write_slot_cb, &ctx);
    if (!ctx.name_matched) return -1;

    fat32_sector_t sec;
    fs->dev->read_blocks(fs->dev, sec.raw, (uint32_t)ctx.free_sector_lba, 1);
    sec.entries[ctx.free_slot].file_size = new_size;
    if (set_first) {
        sec.entries[ctx.free_slot].fst_clus_hi = (uint16_t)(set_first >> 16);
        sec.entries[ctx.free_slot].fst_clus_lo = (uint16_t)(set_first & 0xFFFF);
    }
    fs->dev->write_blocks(fs->dev, sec.raw, (uint32_t)ctx.free_sector_lba, 1);
    return 0;
}

static int fat32_write_file_nolock(fat32_fs_t *fs, const char *path, const void *buf, uint32_t size) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    size_t plen = strlen(path);
    if (plen > 0 && path[plen - 1] == '/') {
        printk("[FAT32] Device '%s': '%s' names a directory, not a file\n",
               fs->dev->name ? fs->dev->name : "unknown", path);
        return -1;
    }
    char target_name[64];
    uint32_t parent_clus = fat32_get_parent_cluster(fs, path, target_name);
    if (parent_clus == 0 || target_name[0] == '\0') {
        /* Everything below this point can fail for a reason the caller cannot
         * see: the shell's `write` reports a bare `#f`, and a driver that is
         * otherwise talkative about refusals said nothing at all -- which is
         * how a full directory came to be investigated as a space problem
         * (plan/open_issues.md, 2026-09-17). Each failure now names itself. */
        printk("[FAT32] Device '%s': no directory to create '%s' in\n",
               fs->dev->name ? fs->dev->name : "unknown", path);
        return -1;
    }

    char name83[11];
    filename_to_83(target_name, name83);

    write_slot_ctx_t ctx = { .name83 = name83, .free_sector_lba = -1, .free_slot = 0, .name_matched = false };
    fat32_scan_dir(fs, parent_clus, write_slot_cb, &ctx);
    if (dir_claim_slot(fs, parent_clus, &ctx) != 0) return -1;

    /* Overwriting an existing file: free its old cluster chain first so
     * the old clusters don't leak (see B8 in
     * plan/completed/2026-08-07_review_and_remediation.md) -- fat32_write_file()
     * always allocated a brand new chain and pointed the entry at it
     * without ever freeing what it used to point at. */
    if (ctx.name_matched) {
        uint32_t old_clus = ((uint32_t)ctx.existing.fst_clus_hi << 16) | ctx.existing.fst_clus_lo;
        if (old_clus) fat32_free_chain(fs, old_clus);
    }

    /* One cluster per bytes_per_cluster, every sector of it written. Until
     * 38.0 this allocated a cluster per 512 bytes and wrote only its first
     * sector -- correct where a cluster *is* one sector (every volume this
     * tree formats), and on a PC-formatted card (32 KB clusters) a file read
     * back with bytes 512.. taken from the unwritten rest of the first
     * cluster. The VFS never reached it with data (it creates empty, then
     * writes through fat32_write_at()), which is why nothing noticed. */
    uint32_t bpc = fs->bytes_per_cluster ? fs->bytes_per_cluster : 512;
    uint32_t sec_per_clus = fs->bpb.sec_per_clus ? fs->bpb.sec_per_clus : 1;
    /* An empty file has no cluster at all (first cluster 0), as the FAT
     * spec has it; fsck.fat truncates anything else. fat32_write_at() gives
     * it one on its first write. */
    uint32_t clusters_needed = (size + bpc - 1) / bpc;

    uint32_t first_cluster = 0;
    uint32_t cur_clus = 0;

    for (uint32_t c = 0; c < clusters_needed; c++) {
        uint32_t next_clus = fat_alloc_cluster(fs);
        if (next_clus == 0) {
            printk("[FAT32] Device '%s': volume full writing '%s' (%u of %u "
                   "clusters placed)\n", fs->dev->name ? fs->dev->name : "unknown",
                   path, (unsigned int)c, (unsigned int)clusters_needed);
            /* Not leaked: the clusters placed so far are freed again. An
             * entry being overwritten already lost its old chain above, so
             * it is left empty rather than pointing at freed clusters that
             * the next allocation would cross-link. */
            if (first_cluster) fat32_free_chain(fs, first_cluster);
            if (ctx.name_matched) {
                fat32_sector_t sector;
                if (fs->dev->read_blocks(fs->dev, sector.raw, (uint32_t)ctx.free_sector_lba, 1) == 0) {
                    sector.entries[ctx.free_slot].fst_clus_hi = 0;
                    sector.entries[ctx.free_slot].fst_clus_lo = 0;
                    sector.entries[ctx.free_slot].file_size = 0;
                    fs->dev->write_blocks(fs->dev, sector.raw, (uint32_t)ctx.free_sector_lba, 1);
                }
            }
            return -1;
        }

        if (c == 0) {
            first_cluster = next_clus;
        } else {
            fat_set_entry(fs, cur_clus, next_clus);
        }
        cur_clus = next_clus;

        uint32_t data_lba = cluster_to_lba(fs, cur_clus);
        for (uint32_t sct = 0; sct < sec_per_clus; sct++) {
            uint32_t offset = c * bpc + sct * 512;
            if (offset >= size && sct > 0) break;   /* past EOF: never read */
            uint32_t chunk = (size > offset) ? (size - offset) : 0;
            if (chunk > 512) chunk = 512;
            if (buf && chunk == 512) {
                fs->dev->write_blocks(fs->dev, (const uint8_t *)buf + offset, data_lba + sct, 1);
                continue;
            }
            uint8_t data_sec[512];
            memset(data_sec, 0, 512);
            if (buf && chunk > 0) memcpy(data_sec, (const uint8_t *)buf + offset, chunk);
            fs->dev->write_blocks(fs->dev, data_sec, data_lba + sct, 1);
        }
    }

    fat32_sector_t sector;
    fs->dev->read_blocks(fs->dev, sector.raw, (uint32_t)ctx.free_sector_lba, 1);
    fat32_dir_entry_t *entries = sector.entries;
    memcpy(entries[ctx.free_slot].name, name83, 11);
    entries[ctx.free_slot].attr = FAT32_ATTR_ARCHIVE;
    entries[ctx.free_slot].fst_clus_hi = (uint16_t)(first_cluster >> 16);
    entries[ctx.free_slot].fst_clus_lo = (uint16_t)(first_cluster & 0xFFFF);
    entries[ctx.free_slot].file_size = size;
    fs->dev->write_blocks(fs->dev, sector.raw, (uint32_t)ctx.free_sector_lba, 1);
    return 0;
}

int fat32_write_file(fat32_fs_t *fs, const char *path, const void *buf, uint32_t size) {
    if (!fs) return -1;
    ylock_acquire(&fs->lock);
    int r = fat32_write_file_nolock(fs, path, buf, size);
    ylock_release(&fs->lock);
    return r;
}

/* Writes `count` bytes at byte `offset` within an existing file: walks (or
 * extends) the cluster chain to `offset`, then fills sector-by-sector via
 * read-modify-write for any bytes landing in an already-allocated sector,
 * and allocates fresh (zeroed) clusters for anything past the current
 * chain -- so writing past EOF zero-fills the gap rather than leaving
 * garbage, matching ordinary sparse-write behavior. Never truncates: a
 * write that lands entirely within the existing size just patches those
 * bytes; file_size only ever grows here. The file must already exist with
 * at least one allocated cluster (see fat32_truncate() for how to get an
 * empty-but-allocated file to extend); a write at a nonzero offset to a
 * file that doesn't exist yet, or has never had a cluster allocated, isn't
 * supported (there's no chain to extend without an offset==0 starting
 * point). Returns bytes written, or -1. */
int fat32_write_at(fat32_fs_t *fs, const char *path, const void *buf, uint32_t count, uint64_t offset,
                   fat32_cursor_t *cur) {
    if (!fs || !path || !buf || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    if (count == 0) return 0;
    fat32_cursor_t local = {0};
    if (!cur) cur = &local;

    ylock_acquire(&fs->lock);
    fat32_dir_entry_t entry;
    if (fat32_find_file(fs, path, &entry) != 0) {
        int r = -1;
        if (offset == 0) r = (fat32_write_file(fs, path, buf, count) == 0) ? (int)count : -1;
        ylock_release(&fs->lock);
        return r;
    }
    uint32_t first_cluster = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
    uint32_t bpc = fs->bytes_per_cluster ? fs->bytes_per_cluster : 512;
    bool new_first = false;
    if (first_cluster == 0) {
        /* An empty file's first write gives it its first cluster. Zeroed
         * only when the write starts past it, since then part of it is a
         * gap that reads back as zeros. */
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
    }

    /* Walk (or extend, zero-filled) to the cluster containing `offset`. */
    uint32_t index = (uint32_t)(offset / bpc);
    uint32_t cluster = chain_seek(fs, first_cluster, index, cur, true);
    if (cluster == 0) {
        /* Out of space on the way to `offset`. A first cluster given to an
         * empty file just now is recorded anyway, so nothing leaks. */
        if (new_first) fat32_update_entry(fs, path, entry.file_size, first_cluster);
        ylock_release(&fs->lock);
        return -1;
    }

    uint32_t offset_in_cluster = (uint32_t)(offset % bpc);
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t written = 0;

    while (written < count) {
        while (offset_in_cluster < bpc && written < count) {
            uint32_t sector_in_cluster = offset_in_cluster / 512;
            uint32_t offset_in_sector = offset_in_cluster % 512;
            uint32_t lba = cluster_to_lba(fs, cluster) + sector_in_cluster;

            uint32_t space = 512 - offset_in_sector;
            uint32_t chunk = (count - written) < space ? (count - written) : space;
            if (chunk == 512) {
                /* A whole sector replaces what was there: no read needed. */
                fs->dev->write_blocks(fs->dev, src + written, lba, 1);
            } else {
                uint8_t sec[512];
                fs->dev->read_blocks(fs->dev, sec, lba, 1);
                memcpy(sec + offset_in_sector, src + written, chunk);
                fs->dev->write_blocks(fs->dev, sec, lba, 1);
            }

            written += chunk;
            offset_in_cluster += chunk;
        }
        if (written < count) {
            /* Extending here does not zero the new cluster first: every byte
             * up to the new EOF is about to be written, and what lies past
             * EOF is never read. */
            uint32_t next = fat_get_entry(fs, cluster);
            if (next >= 0x0FFFFFF8) {
                uint32_t new_clus = fat_alloc_cluster(fs);
                if (new_clus == 0) break; /* out of space; keep whatever was written */
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
    if (new_size > entry.file_size || new_first) {
        fat32_update_entry(fs, path, (uint32_t)(new_size > entry.file_size ? new_size : entry.file_size),
                           new_first ? first_cluster : 0);
    }
    ylock_release(&fs->lock);
    return (int)written;
}

/* Frees an existing file's cluster chain and resets it to empty (size 0,
 * no first cluster) without deleting its directory entry -- the FAT32-level
 * primitive behind VFS_O_TRUNC. Reuses fat32_write_file()'s own "free the
 * old chain, then allocate a chain sized to `size`" path; for size 0 that
 * is no chain at all since 38.0, and fat32_write_at() allocates the first
 * cluster on the first write. */
int fat32_truncate(fat32_fs_t *fs, const char *path) {
    return fat32_write_file(fs, path, NULL, 0);
}

/* Appends `len` bytes to the end of an existing file. Used by the shell's
 * command-history log, which used to read, concatenate, and fully rewrite
 * (reallocating a brand new cluster chain for) its entire accumulated
 * content on every single command (B8) -- with the write-side leak that
 * caused now fixed, that no longer corrupts free space, but it's still
 * O(session length) work per keystroke; this makes it O(1). Falls back to
 * a plain fat32_write_file() for a file that doesn't exist yet or is
 * currently empty (matching fat32_write_at()'s own offset==0 fallback). */
int fat32_append_file(fat32_fs_t *fs, const char *path, const void *buf, uint32_t len) {
    if (!fs || !path || !buf || len == 0) return 0;

    ylock_acquire(&fs->lock);
    int r;
    fat32_dir_entry_t entry;
    if (fat32_find_file(fs, path, &entry) < 0) {
        r = (fat32_write_file(fs, path, buf, len) == 0) ? (int)len : -1;
    } else {
        r = fat32_write_at(fs, path, buf, len, entry.file_size, NULL);
    }
    ylock_release(&fs->lock);
    return r;
}

/* --- fat32_mkdir: free-slot search (read-only match, reuses write_slot_cb) --- */

static int fat32_mkdir_nolock(fat32_fs_t *fs, const char *path) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    char new_dir_name[64];
    uint32_t parent_clus = fat32_get_parent_cluster(fs, path, new_dir_name);
    if (parent_clus == 0 || new_dir_name[0] == '\0') return -1;

    fat32_dir_entry_t existing;
    if (fat32_find_file(fs, path, &existing) >= 0) return -1;

    char name83[11];
    filename_to_83(new_dir_name, name83);

    write_slot_ctx_t ctx = { .name83 = name83, .free_sector_lba = -1, .free_slot = 0, .name_matched = false };
    fat32_scan_dir(fs, parent_clus, write_slot_cb, &ctx);
    if (dir_claim_slot(fs, parent_clus, &ctx) != 0) return -1;

    uint32_t new_clus = fat_alloc_cluster(fs);
    if (new_clus == 0) {
        printk("[FAT32] Device '%s': no free cluster for the new directory '%s'\n",
               fs->dev->name ? fs->dev->name : "unknown", new_dir_name);
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

    fs->dev->write_blocks(fs->dev, dir_sec.raw, cluster_to_lba(fs, new_clus), 1);

    fat32_sector_t sector;
    fs->dev->read_blocks(fs->dev, sector.raw, (uint32_t)ctx.free_sector_lba, 1);
    fat32_dir_entry_t *entries = sector.entries;
    memcpy(entries[ctx.free_slot].name, name83, 11);
    entries[ctx.free_slot].attr = FAT32_ATTR_DIRECTORY;
    entries[ctx.free_slot].fst_clus_hi = (uint16_t)(new_clus >> 16);
    entries[ctx.free_slot].fst_clus_lo = (uint16_t)(new_clus & 0xFFFF);
    entries[ctx.free_slot].file_size = 0;
    fs->dev->write_blocks(fs->dev, sector.raw, (uint32_t)ctx.free_sector_lba, 1);

    printk("[FAT32] Device '%s': Subdirectory created: '%s' (Cluster %u)\n",
           fs->dev->name ? fs->dev->name : "unknown", new_dir_name, (unsigned int)new_clus);
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

typedef struct {
    const char *name83;
    bool want_dir;      /* the entry must be a directory (rmdir), or must not be */
    bool removed;
    bool wrong_kind;
} remove_ctx_t;

static bool remove_file_cb(fat32_fs_t *fs, uint32_t sector_lba,
                            fat32_dir_entry_t *entries, int count, void *vctx) {
    remove_ctx_t *ctx = (remove_ctx_t *)vctx;
    for (int i = 0; i < count; i++) {
        if (entries[i].name[0] == 0x00) return true;
        if (dir_entry_is_skippable(&entries[i])) continue;
        if (memcmp(entries[i].name, ctx->name83, 11) == 0) {
            if (!(entries[i].attr & FAT32_ATTR_DIRECTORY) != !ctx->want_dir) {
                ctx->wrong_kind = true;
                return true;
            }
            uint32_t clus = ((uint32_t)entries[i].fst_clus_hi << 16) | entries[i].fst_clus_lo;
            if (clus) fat32_free_chain(fs, clus); // B8: free the file's data, don't just orphan it
            entries[i].name[0] = 0xE5; // Mark deleted
            fs->dev->write_blocks(fs->dev, entries, sector_lba, 1);
            ctx->removed = true;
            return true;
        }
    }
    return false;
}

/* Removes the entry `path` names, which must be a directory if `want_dir`
 * and must not be one otherwise. A directory removed as a file used to
 * succeed with its children still in it: their clusters were orphaned for
 * good, counted as used and reachable from nowhere (`rm /ram0/dir`). Only
 * fat32_rmdir(), after checking the directory is empty, removes one. */
static int fat32_remove_entry_nolock(fat32_fs_t *fs, const char *path, bool want_dir) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    char target_name[64];
    uint32_t parent_clus = fat32_get_parent_cluster(fs, path, target_name);
    if (parent_clus == 0 || target_name[0] == '\0') return -1;

    char name83[11];
    filename_to_83(target_name, name83);

    remove_ctx_t ctx = { .name83 = name83, .want_dir = want_dir };
    fat32_scan_dir(fs, parent_clus, remove_file_cb, &ctx);
    if (ctx.wrong_kind && !want_dir)
        cprintf("rm: '%s' is a directory; use rmdir\n", path);
    return ctx.removed ? 0 : -1;
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
    if (fat32_find_file(fs, path, &entry) < 0) return -1;

    if (!(entry.attr & FAT32_ATTR_DIRECTORY)) {
        cprintf("rmdir: '%s' is not a directory\n", path);
        return -1;
    }

    uint32_t dir_clus = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
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

    /* Free the directory's own cluster chain in the FAT, then mark its
     * entry in the parent directory deleted (fat32_remove_entry_nolock() also
     * frees whatever's left of the chain -- fat32_free_chain() tolerates
     * being pointed at an already-freed chain, see its own comment). */
    fat32_free_chain(fs, dir_clus);
    return fat32_remove_entry_nolock(fs, path, true);
}

int fat32_rmdir(fat32_fs_t *fs, const char *path) {
    if (!fs) return -1;
    ylock_acquire(&fs->lock);
    int r = fat32_rmdir_nolock(fs, path);
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


/* --- fat32_list_dir --- */

static bool list_dir_cb(fat32_fs_t *fs, uint32_t sector_lba,
                         fat32_dir_entry_t *entries, int count, void *vctx) {
    (void)fs; (void)sector_lba;
    int *out_count = (int *)vctx;
    for (int i = 0; i < count; i++) {
        if (entries[i].name[0] == 0x00) return true;
        if ((uint8_t)entries[i].name[0] == 0xE5) continue;
        if ((entries[i].attr & 0x0F) == 0x0F) continue; // Skip VFAT Long File Name (LFN) metadata
        if (entries[i].attr & FAT32_ATTR_VOLUME_ID) continue; // Skip Volume Label entry

        char namebuf[13];
        memcpy(namebuf, entries[i].name, 11);
        namebuf[11] = '\0';
        bool is_dir = (entries[i].attr & FAT32_ATTR_DIRECTORY) != 0;
        cprintf("%s  %12u  0x%02x   %s\n",
               namebuf,
               (unsigned int)entries[i].file_size,
               entries[i].attr,
               is_dir ? "<DIR>" : "<FILE>");
        (*out_count)++;
    }
    return false;
}

static void fat32_list_dir_nolock(fat32_fs_t *fs, const char *path) {
    if (!fs || !fs->dev || !fs->dev->read_blocks) return;

    uint32_t target_clus = fs->root_dir_cluster;
    if (path && *path && strcmp(path, "/") != 0) {
        fat32_dir_entry_t entry;
        if (fat32_find_file(fs, path, &entry) < 0) {
            cprintf("ls: path '%s' not found\n", path);
            return;
        }
        if (!(entry.attr & FAT32_ATTR_DIRECTORY)) {
            cprintf("ls: '%s' is not a directory\n", path);
            return;
        }
        target_clus = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
        if (target_clus == 0) target_clus = fs->root_dir_cluster;
    }

    cprintf("\nDirectory Listing (FAT32):\n");
    cprintf("Name        Size (Bytes)  Attr   Type\n");
    cprintf("----------  ------------  -----  -----\n");

    int count = 0;
    fat32_scan_dir(fs, target_clus, list_dir_cb, &count);

    if (count == 0) {
        cprintf("(empty directory)\n");
    }
    cprintf("\n");
}

void fat32_list_dir(fat32_fs_t *fs, const char *path) {
    if (!fs) return;
    ylock_acquire(&fs->lock);
    fat32_list_dir_nolock(fs, path);
    ylock_release(&fs->lock);
}

/* --- fat32_readdir --- */

typedef struct {
    uint32_t target_index;
    uint32_t current_index;
    char *name_out;
    uint32_t name_max;
    fat32_dir_entry_t *out_entry;
    bool found;
} readdir_ctx_t;

static bool readdir_cb(fat32_fs_t *fs, uint32_t sector_lba,
                        fat32_dir_entry_t *entries, int count, void *vctx) {
    (void)fs; (void)sector_lba;
    readdir_ctx_t *ctx = (readdir_ctx_t *)vctx;
    for (int i = 0; i < count; i++) {
        if (entries[i].name[0] == 0x00) return true; // end of directory
        if ((uint8_t)entries[i].name[0] == 0xE5) continue; // deleted
        if ((entries[i].attr & 0x0F) == 0x0F) continue;    // VFAT LFN metadata
        if (entries[i].attr & FAT32_ATTR_VOLUME_ID) continue;

        if (ctx->current_index == ctx->target_index) {
            if (ctx->out_entry) *ctx->out_entry = entries[i];
            if (ctx->name_out && ctx->name_max > 0) {
                fat83_to_display_name(entries[i].name, ctx->name_out, ctx->name_max);
            }
            ctx->found = true;
            return true;
        }
        ctx->current_index++;
    }
    return false;
}

static int fat32_readdir_nolock(fat32_fs_t *fs, uint32_t dir_cluster, uint32_t index,
                   char *name_out, uint32_t name_max, fat32_dir_entry_t *out_entry) {
    if (!fs) return -1;
    readdir_ctx_t ctx = {
        .target_index = index, .current_index = 0,
        .name_out = name_out, .name_max = name_max,
        .out_entry = out_entry, .found = false,
    };
    fat32_scan_dir(fs, dir_cluster, readdir_cb, &ctx);
    return ctx.found ? 0 : -1;
}

int fat32_readdir(fat32_fs_t *fs, uint32_t dir_cluster, uint32_t index,
                   char *name_out, uint32_t name_max, fat32_dir_entry_t *out_entry) {
    if (!fs) return -1;
    ylock_acquire(&fs->lock);
    int r = fat32_readdir_nolock(fs, dir_cluster, index, name_out, name_max, out_entry);
    ylock_release(&fs->lock);
    return r;
}
