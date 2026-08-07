#include "fs/fat32.h"
#include "kernel/printk.h"
#include <string.h>

static void filename_to_83(const char *src, char *dst) {
    if (!src || !dst) return;
    memset(dst, ' ', 11);
    int i = 0;
    while (*src && *src != '.' && *src != '/' && i < 8) {
        char c = *src++;
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[i++] = c;
    }
    if (*src == '.') {
        src++;
        int j = 8;
        while (*src && *src != '/' && j < 11) {
            char c = *src++;
            if (c >= 'a' && c <= 'z') c -= 32;
            dst[j++] = c;
        }
    }
}

static uint32_t cluster_to_lba(fat32_fs_t *fs, uint32_t cluster) {
    if (!fs) return 0;
    return fs->data_start_sector + (cluster - 2) * fs->bpb.sec_per_clus;
}

/* Read a 32-bit FAT entry for the given cluster */
static uint32_t fat_get_entry(fat32_fs_t *fs, uint32_t cluster) {
    if (!fs || !fs->dev || !fs->dev->read_blocks) return 0x0FFFFFFF;
    uint32_t fat_sector = fs->fat_start_sector + (cluster * 4) / 512;
    uint32_t fat_offset = (cluster * 4) % 512;
    uint8_t fat_sec[512];
    fs->dev->read_blocks(fs->dev, fat_sec, fat_sector, 1);
    return ((uint32_t *)fat_sec)[fat_offset / 4] & 0x0FFFFFFF;
}

/* Write a 32-bit FAT entry for the given cluster (both FAT1 and FAT2) */
static void fat_set_entry(fat32_fs_t *fs, uint32_t cluster, uint32_t value) {
    if (!fs || !fs->dev || !fs->dev->write_blocks) return;
    uint32_t fat_sector = fs->fat_start_sector + (cluster * 4) / 512;
    uint32_t fat_offset = (cluster * 4) % 512;
    uint8_t fat_sec[512];
    fs->dev->read_blocks(fs->dev, fat_sec, fat_sector, 1);
    ((uint32_t *)fat_sec)[fat_offset / 4] = value;
    fs->dev->write_blocks(fs->dev, fat_sec, fat_sector, 1); // FAT1
    if (fs->bpb.num_fats > 1) {
        /* The second FAT copy starts fat_sz32 sectors after the first, not
         * a hardcoded +8 -- that only happened to be correct for volumes
         * this codebase's own fat32_format() created (which always uses an
         * 8-sector FAT). Any normally-sized FAT32 volume (a real SD card
         * formatted by a PC) has a much larger FAT, and the hardcoded
         * offset silently wrote cluster-chain updates into whatever
         * unrelated sector was 8 sectors after FAT1 instead of into FAT2
         * (see B9 in plan/2026-08-07_review_and_remediation.md). */
        fs->dev->write_blocks(fs->dev, fat_sec, fat_sector + fs->bpb.fat_sz32, 1); // FAT2
    }
}

/* Frees an entire cluster chain by zeroing each FAT entry from
 * first_cluster onward. Tolerates being pointed at an already-partially- or
 * fully-freed chain (stops as soon as it reads a 0 entry) and never touches
 * the reserved cluster 0/1 FAT entries, so it's safe to call unconditionally
 * -- e.g. fat32_rmdir() frees a directory's own chain and then calls
 * fat32_remove_file(), which (after the B8 fix below) also frees
 * whatever's left of the same chain. Also guards against a corrupt
 * self-referential chain looping forever. */
static void fat32_free_chain(fat32_fs_t *fs, uint32_t first_cluster) {
    uint32_t clus = first_cluster;
    while (clus >= 2 && clus < 0x0FFFFFF8) {
        uint32_t next = fat_get_entry(fs, clus);
        fat_set_entry(fs, clus, 0x00000000);
        if (next == clus) break;
        clus = next;
    }
}

/* Dynamically allocate a free cluster from the FAT table */
static uint32_t fat_alloc_cluster(fat32_fs_t *fs) {
    if (!fs || fs->bpb.sec_per_clus == 0) return 0;
    /* tot_sec32 is a *sector* count, not a cluster count -- clusters are
     * numbered from 2 and only span the data region (total sectors minus
     * the reserved+FAT area), which is always fewer than tot_sec32.
     * Iterating cluster numbers up to tot_sec32 (the previous behavior)
     * scanned past the real data region into the FAT2 copy (or beyond the
     * device entirely), could return a "free" cluster number that actually
     * pointed past the end of the volume, and fat_set_entry() would then
     * write a chain-link into whatever unrelated storage that cluster
     * number's FAT-entry offset landed on (see B9 in
     * plan/2026-08-07_review_and_remediation.md). */
    uint32_t total_sec = fs->bpb.tot_sec32 ? fs->bpb.tot_sec32 : fs->bpb.tot_sec16;
    uint32_t fat_area_sec = fs->bpb.reserved_sec_cnt + (uint32_t)fs->bpb.num_fats * fs->bpb.fat_sz32;
    if (total_sec <= fat_area_sec) return 0;
    uint32_t data_sec = total_sec - fat_area_sec;
    uint32_t total_clusters = data_sec / fs->bpb.sec_per_clus;
    uint32_t max_cluster = total_clusters + 2; /* exclusive upper bound; clusters start at 2 */

    for (uint32_t c = 2; c < max_cluster; c++) {
        if (fat_get_entry(fs, c) == 0x00000000) {
            fat_set_entry(fs, c, 0x0FFFFFFF);
            return c;
        }
    }
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
 * codebase's own fat32_format() always uses sec_per_clus = 1, but any
 * normally PC-formatted FAT32 card typically uses 8-64 sectors per cluster
 * (see B9 in plan/2026-08-07_review_and_remediation.md). Two of the
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
            uint8_t sector[512];
            uint32_t lba = cluster_to_lba(fs, clus_iter) + s;
            fs->dev->read_blocks(fs->dev, sector, lba, 1);
            if (fn(fs, lba, (fat32_dir_entry_t *)sector, 16, ctx)) return;
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

/* Extract parent cluster and target file/directory name from a path */
static uint32_t fat32_get_parent_cluster(fat32_fs_t *fs, const char *path, char *out_name) {
    if (!fs || !path) return 0;
    while (*path == '/') path++;

    char path_copy[256];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    uint32_t cur_clus = fs->root_dir_cluster;
    char *curr = path_copy;

    while (*curr) {
        char *slash = strchr(curr, '/');
        if (!slash) {
            if (out_name) strncpy(out_name, curr, 63);
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
    if (!dev || !dev->write_blocks) return -1;
    uint8_t sector[512];
    memset(sector, 0, 512);

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector;
    bpb->jmp_boot[0] = 0xEB; bpb->jmp_boot[1] = 0x58; bpb->jmp_boot[2] = 0x90;
    memcpy(bpb->oem_name, "MSWIN4.1", 8);
    bpb->bytes_per_sec = 512;
    bpb->sec_per_clus = 1;
    bpb->reserved_sec_cnt = 32;
    bpb->num_fats = 2;
    bpb->tot_sec32 = dev->num_blocks;
    bpb->fat_sz32 = 8;
    bpb->root_clus = 2;
    bpb->boot_sig = 0x29;
    memcpy(bpb->vol_lab, "LUGALOS_FAT", 11);
    memcpy(bpb->fil_sys_type, "FAT32   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xAA;

    dev->write_blocks(dev, sector, 0, 1);

    memset(sector, 0, 512);
    uint32_t *fat = (uint32_t *)sector;
    fat[0] = 0x0FFFFFF8;
    fat[1] = 0x0FFFFFFF;
    fat[2] = 0x0FFFFFFF;

    dev->write_blocks(dev, sector, 32, 1);
    dev->write_blocks(dev, sector, 40, 1);

    memset(sector, 0, 512);
    dev->write_blocks(dev, sector, 48, 1);

    printk("[FAT32] Device '%s': Volume formatted cleanly as FAT32.\n", dev->name ? dev->name : "unknown");
    return 0;
}

int fat32_init(fat32_fs_t *fs, block_dev_t *dev) {
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
         * plan/2026-08-07_review_and_remediation.md). Report the volume as
         * unmounted instead; fat32_format() is still available and is now
         * only ever invoked explicitly (via the `format` Lisp primitive,
         * or vfs_mount_ramdisk()'s own deliberate fallback for the RAM
         * disk, which is expected to start blank every boot). */
        printk("[FAT32] Device '%s': No valid FAT32 volume found (not mounted). "
               "Use (format \"<path>\") to initialize it.\n",
               dev->name ? dev->name : "unknown");
        return -1;
    }

    fs->fat_start_sector = partition_lba + fs->bpb.reserved_sec_cnt;
    fs->data_start_sector = fs->fat_start_sector + (fs->bpb.num_fats * fs->bpb.fat_sz32);
    fs->root_dir_cluster = fs->bpb.root_clus;
    fs->bytes_per_cluster = fs->bpb.sec_per_clus * fs->bpb.bytes_per_sec;

    printk("[FAT32] Device '%s': Volume Mounted. Label: '%.11s', Data LBA: %u\n",
           dev->name ? dev->name : "unknown", fs->bpb.vol_lab, (unsigned int)fs->data_start_sector);
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

int fat32_find_file(fat32_fs_t *fs, const char *path, fat32_dir_entry_t *out_entry) {
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

    find_file_ctx_t ctx = { .name83 = name83, .out_entry = out_entry, .found = false };
    fat32_scan_dir(fs, parent_clus, find_file_cb, &ctx);
    return ctx.found ? 0 : -1;
}

int fat32_read_file(fat32_fs_t *fs, fat32_dir_entry_t *entry, void *buf, uint32_t max_size) {
    if (!fs || !entry || !buf || !fs->dev || !fs->dev->read_blocks) return -1;
    if (max_size == 0) return 0;
    uint32_t cluster = ((uint32_t)entry->fst_clus_hi << 16) | entry->fst_clus_lo;
    /* Reserve the last byte of the caller's buffer for the NUL terminator
     * written below, regardless of what max_size the caller passed. Most
     * callers already pass sizeof(buf) - 1 themselves, but at least one
     * (arch/riscv/common/elf.c) passed sizeof(file_buf) directly, which
     * wrote one byte past the end of its buffer whenever the file being
     * read was >= that size (see B7 in
     * plan/2026-08-07_review_and_remediation.md). Clamping here makes the
     * function safe regardless of what any given caller passes. */
    uint32_t capacity = max_size - 1;
    uint32_t size = entry->file_size < capacity ? entry->file_size : capacity;

    uint32_t bytes_read = 0;
    while (cluster < 0x0FFFFFF8 && bytes_read < size) {
        uint32_t lba = cluster_to_lba(fs, cluster);
        uint8_t data_sec[512];
        fs->dev->read_blocks(fs->dev, data_sec, lba, 1);

        uint32_t chunk = size - bytes_read;
        if (chunk > 512) chunk = 512;
        memcpy((uint8_t *)buf + bytes_read, data_sec, chunk);
        bytes_read += chunk;

        cluster = fat_get_entry(fs, cluster);
    }
    ((char *)buf)[bytes_read] = '\0';
    return (int)bytes_read;
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

int fat32_write_file(fat32_fs_t *fs, const char *path, const void *buf, uint32_t size) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    char target_name[64];
    uint32_t parent_clus = fat32_get_parent_cluster(fs, path, target_name);
    if (parent_clus == 0 || target_name[0] == '\0') return -1;

    char name83[11];
    filename_to_83(target_name, name83);

    write_slot_ctx_t ctx = { .name83 = name83, .free_sector_lba = -1, .free_slot = 0, .name_matched = false };
    fat32_scan_dir(fs, parent_clus, write_slot_cb, &ctx);
    if (ctx.free_sector_lba < 0) return -1; // Directory full

    /* Overwriting an existing file: free its old cluster chain first so
     * the old clusters don't leak (see B8 in
     * plan/2026-08-07_review_and_remediation.md) -- fat32_write_file()
     * always allocated a brand new chain and pointed the entry at it
     * without ever freeing what it used to point at. */
    if (ctx.name_matched) {
        uint32_t old_clus = ((uint32_t)ctx.existing.fst_clus_hi << 16) | ctx.existing.fst_clus_lo;
        if (old_clus) fat32_free_chain(fs, old_clus);
    }

    uint32_t sectors_needed = (size + 511) / 512;
    if (sectors_needed == 0) sectors_needed = 1;

    uint32_t first_cluster = 0;
    uint32_t cur_clus = 0;

    for (uint32_t s = 0; s < sectors_needed; s++) {
        uint32_t next_clus = fat_alloc_cluster(fs);
        if (next_clus == 0) return -1;

        if (s == 0) {
            first_cluster = next_clus;
        } else {
            fat_set_entry(fs, cur_clus, next_clus);
        }
        cur_clus = next_clus;

        uint32_t data_lba = cluster_to_lba(fs, cur_clus);
        uint8_t data_sec[512];
        memset(data_sec, 0, 512);
        uint32_t offset = s * 512;
        uint32_t chunk = (size > offset) ? (size - offset) : 0;
        if (chunk > 512) chunk = 512;
        if (buf && chunk > 0) memcpy(data_sec, (const uint8_t *)buf + offset, chunk);
        fs->dev->write_blocks(fs->dev, data_sec, data_lba, 1);
    }
    fat_set_entry(fs, cur_clus, 0x0FFFFFFF);

    uint8_t sector[512];
    fs->dev->read_blocks(fs->dev, sector, (uint32_t)ctx.free_sector_lba, 1);
    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)sector;
    memcpy(entries[ctx.free_slot].name, name83, 11);
    entries[ctx.free_slot].attr = FAT32_ATTR_ARCHIVE;
    entries[ctx.free_slot].fst_clus_hi = (uint16_t)(first_cluster >> 16);
    entries[ctx.free_slot].fst_clus_lo = (uint16_t)(first_cluster & 0xFFFF);
    entries[ctx.free_slot].file_size = size;
    fs->dev->write_blocks(fs->dev, sector, (uint32_t)ctx.free_sector_lba, 1);
    return 0;
}

/* Appends `len` bytes to the end of an existing file: walks to the last
 * cluster of its chain, fills any space remaining in the already-allocated
 * last sector (read-modify-write), then allocates and writes further
 * clusters only for whatever's left, and finally updates the directory
 * entry's file_size in place. Falls back to a plain fat32_write_file() for
 * a file that doesn't exist yet or is currently empty. Used by the shell's
 * command-history log, which used to read, concatenate, and fully rewrite
 * (reallocating a brand new cluster chain for) its entire accumulated
 * content on every single command (B8) -- with the write-side leak above
 * now fixed, that no longer corrupts free space, but it's still O(session
 * length) work per keystroke; this makes it O(1). */
int fat32_append_file(fat32_fs_t *fs, const char *path, const void *buf, uint32_t len) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    if (!buf || len == 0) return 0;

    fat32_dir_entry_t entry;
    if (fat32_find_file(fs, path, &entry) < 0) {
        return (fat32_write_file(fs, path, buf, len) == 0) ? (int)len : -1;
    }

    uint32_t first_cluster = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
    uint32_t old_size = entry.file_size;
    if (first_cluster == 0 || old_size == 0) {
        return (fat32_write_file(fs, path, buf, len) == 0) ? (int)len : -1;
    }

    uint32_t last_cluster = first_cluster;
    uint32_t next;
    while ((next = fat_get_entry(fs, last_cluster)) < 0x0FFFFFF8) {
        last_cluster = next;
    }

    uint32_t bpc = fs->bytes_per_cluster ? fs->bytes_per_cluster : 512;
    uint32_t used_in_last = old_size % bpc;
    if (used_in_last == 0) used_in_last = bpc; /* last cluster is exactly full */

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t written = 0;

    /* 1. Fill whatever space remains in the already-allocated last cluster,
     *    sector by sector -- a sector may already hold real data in its
     *    first part, so this must read-modify-write, not overwrite. */
    uint32_t offset_in_cluster = used_in_last;
    while (offset_in_cluster < bpc && written < len) {
        uint32_t sector_in_cluster = offset_in_cluster / 512;
        uint32_t offset_in_sector = offset_in_cluster % 512;
        uint32_t lba = cluster_to_lba(fs, last_cluster) + sector_in_cluster;

        uint8_t sec[512];
        fs->dev->read_blocks(fs->dev, sec, lba, 1);
        uint32_t space = 512 - offset_in_sector;
        uint32_t chunk = (len - written) < space ? (len - written) : space;
        memcpy(sec + offset_in_sector, src + written, chunk);
        fs->dev->write_blocks(fs->dev, sec, lba, 1);

        written += chunk;
        offset_in_cluster += chunk;
    }

    /* 2. Allocate new clusters for whatever's left. */
    uint32_t cur_clus = last_cluster;
    while (written < len) {
        uint32_t new_clus = fat_alloc_cluster(fs);
        if (new_clus == 0) break; /* out of space; keep whatever was written */
        fat_set_entry(fs, cur_clus, new_clus);
        cur_clus = new_clus;

        for (uint32_t s = 0; s < fs->bpb.sec_per_clus && written < len; s++) {
            uint8_t sec[512];
            memset(sec, 0, 512);
            uint32_t chunk = (len - written) < 512 ? (len - written) : 512;
            memcpy(sec, src + written, chunk);
            fs->dev->write_blocks(fs->dev, sec, cluster_to_lba(fs, new_clus) + s, 1);
            written += chunk;
        }
    }
    if (cur_clus != last_cluster) {
        fat_set_entry(fs, cur_clus, 0x0FFFFFFF);
    }

    /* 3. Update the directory entry's file_size in place. */
    char target_name[64];
    uint32_t parent_clus = fat32_get_parent_cluster(fs, path, target_name);
    if (parent_clus != 0 && target_name[0] != '\0') {
        char name83[11];
        filename_to_83(target_name, name83);
        uint32_t new_size = old_size + written;

        write_slot_ctx_t find_ctx = { .name83 = name83, .free_sector_lba = -1, .free_slot = 0, .name_matched = false };
        fat32_scan_dir(fs, parent_clus, write_slot_cb, &find_ctx);
        if (find_ctx.name_matched) {
            uint8_t sec[512];
            fs->dev->read_blocks(fs->dev, sec, (uint32_t)find_ctx.free_sector_lba, 1);
            ((fat32_dir_entry_t *)sec)[find_ctx.free_slot].file_size = new_size;
            fs->dev->write_blocks(fs->dev, sec, (uint32_t)find_ctx.free_sector_lba, 1);
        }
    }

    return (int)written;
}

/* --- fat32_mkdir: free-slot search (read-only match, reuses write_slot_cb) --- */

int fat32_mkdir(fat32_fs_t *fs, const char *path) {
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
    if (ctx.free_sector_lba < 0) return -1; // Directory full

    uint32_t new_clus = fat_alloc_cluster(fs);
    if (new_clus == 0) return -1;

    uint8_t dir_sec[512];
    memset(dir_sec, 0, 512);

    fat32_dir_entry_t *dot = (fat32_dir_entry_t *)&dir_sec[0];
    memcpy(dot->name, ".          ", 11);
    dot->attr = FAT32_ATTR_DIRECTORY;
    dot->fst_clus_hi = (uint16_t)(new_clus >> 16);
    dot->fst_clus_lo = (uint16_t)(new_clus & 0xFFFF);

    fat32_dir_entry_t *dotdot = (fat32_dir_entry_t *)&dir_sec[32];
    memcpy(dotdot->name, "..         ", 11);
    dotdot->attr = FAT32_ATTR_DIRECTORY;
    uint32_t up_clus = (parent_clus == fs->root_dir_cluster) ? 0 : parent_clus;
    dotdot->fst_clus_hi = (uint16_t)(up_clus >> 16);
    dotdot->fst_clus_lo = (uint16_t)(up_clus & 0xFFFF);

    fs->dev->write_blocks(fs->dev, dir_sec, cluster_to_lba(fs, new_clus), 1);

    uint8_t sector[512];
    fs->dev->read_blocks(fs->dev, sector, (uint32_t)ctx.free_sector_lba, 1);
    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)sector;
    memcpy(entries[ctx.free_slot].name, name83, 11);
    entries[ctx.free_slot].attr = FAT32_ATTR_DIRECTORY;
    entries[ctx.free_slot].fst_clus_hi = (uint16_t)(new_clus >> 16);
    entries[ctx.free_slot].fst_clus_lo = (uint16_t)(new_clus & 0xFFFF);
    entries[ctx.free_slot].file_size = 0;
    fs->dev->write_blocks(fs->dev, sector, (uint32_t)ctx.free_sector_lba, 1);

    printk("[FAT32] Device '%s': Subdirectory created: '%s' (Cluster %u)\n",
           fs->dev->name ? fs->dev->name : "unknown", new_dir_name, (unsigned int)new_clus);
    return 0;
}

/* --- fat32_remove_file --- */

typedef struct {
    const char *name83;
    bool removed;
} remove_ctx_t;

static bool remove_file_cb(fat32_fs_t *fs, uint32_t sector_lba,
                            fat32_dir_entry_t *entries, int count, void *vctx) {
    remove_ctx_t *ctx = (remove_ctx_t *)vctx;
    for (int i = 0; i < count; i++) {
        if (entries[i].name[0] == 0x00) return true;
        if ((uint8_t)entries[i].name[0] == 0xE5) continue;
        if (memcmp(entries[i].name, ctx->name83, 11) == 0) {
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

int fat32_remove_file(fat32_fs_t *fs, const char *path) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    char target_name[64];
    uint32_t parent_clus = fat32_get_parent_cluster(fs, path, target_name);
    if (parent_clus == 0 || target_name[0] == '\0') return -1;

    char name83[11];
    filename_to_83(target_name, name83);

    remove_ctx_t ctx = { .name83 = name83, .removed = false };
    fat32_scan_dir(fs, parent_clus, remove_file_cb, &ctx);
    return ctx.removed ? 0 : -1;
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

int fat32_rmdir(fat32_fs_t *fs, const char *path) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    fat32_dir_entry_t entry;
    if (fat32_find_file(fs, path, &entry) < 0) return -1;

    if (!(entry.attr & FAT32_ATTR_DIRECTORY)) {
        printk("rmdir: '%s' is not a directory\n", path);
        return -1;
    }

    uint32_t dir_clus = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
    if (dir_clus == 0 || dir_clus == fs->root_dir_cluster) {
        printk("rmdir: cannot remove root directory\n");
        return -1;
    }

    empty_check_ctx_t ctx = { .has_real_entries = false };
    fat32_scan_dir(fs, dir_clus, dir_empty_check_cb, &ctx);
    if (ctx.has_real_entries) {
        printk("rmdir: directory '%s' is not empty\n", path);
        return -1;
    }

    /* Free the directory's own cluster chain in the FAT, then mark its
     * entry in the parent directory deleted (fat32_remove_file() also
     * frees whatever's left of the chain -- fat32_free_chain() tolerates
     * being pointed at an already-freed chain, see its own comment). */
    fat32_free_chain(fs, dir_clus);
    return fat32_remove_file(fs, path);
}

int fat32_statfs(fat32_fs_t *fs, uint32_t *total_bytes, uint32_t *free_bytes) {
    if (!fs || !fs->dev) return -1;
    uint32_t total_sec = fs->bpb.tot_sec32 ? fs->bpb.tot_sec32 : fs->bpb.tot_sec16;
    uint32_t total_sz = total_sec * fs->bpb.bytes_per_sec;
    if (total_bytes) *total_bytes = total_sz;

    uint32_t free_clusters = 0;
    uint32_t total_clusters = (total_sec > fs->data_start_sector) ?
        ((total_sec - fs->data_start_sector) / fs->bpb.sec_per_clus) : 0;
    uint32_t fat_sec = fs->fat_start_sector;
    uint32_t fat_buf[128];

    uint32_t checked = 0;
    while (checked < total_clusters && (fat_sec < fs->data_start_sector)) {
        if (fs->dev->read_blocks(fs->dev, fat_buf, fat_sec, 1) != 0) break;
        for (int i = 0; i < 128 && checked < total_clusters; i++, checked++) {
            uint32_t val = fat_buf[i] & 0x0FFFFFFF;
            if (val == 0) free_clusters++;
        }
        fat_sec++;
    }

    if (free_bytes) *free_bytes = free_clusters * fs->bytes_per_cluster;
    return 0;
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
        printk("%s  %12u  0x%02x   %s\n",
               namebuf,
               (unsigned int)entries[i].file_size,
               entries[i].attr,
               is_dir ? "<DIR>" : "<FILE>");
        (*out_count)++;
    }
    return false;
}

void fat32_list_dir(fat32_fs_t *fs, const char *path) {
    if (!fs || !fs->dev || !fs->dev->read_blocks) return;

    uint32_t target_clus = fs->root_dir_cluster;
    if (path && *path && strcmp(path, "/") != 0) {
        fat32_dir_entry_t entry;
        if (fat32_find_file(fs, path, &entry) < 0) {
            printk("ls: path '%s' not found\n", path);
            return;
        }
        if (!(entry.attr & FAT32_ATTR_DIRECTORY)) {
            printk("ls: '%s' is not a directory\n", path);
            return;
        }
        target_clus = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
        if (target_clus == 0) target_clus = fs->root_dir_cluster;
    }

    printk("\nDirectory Listing (FAT32):\n");
    printk("Name        Size (Bytes)  Attr   Type\n");
    printk("----------  ------------  -----  -----\n");

    int count = 0;
    fat32_scan_dir(fs, target_clus, list_dir_cb, &count);

    if (count == 0) {
        printk("(empty directory)\n");
    }
    printk("\n");
}
