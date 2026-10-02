#ifndef LUGALOS_FS_FAT32_H
#define LUGALOS_FS_FAT32_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/block.h"
#include "kernel/lock.h"

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
/* The smallest volume fat32_format() can produce that is actually usable.
 *
 * Its layout spends 32 reserved sectors plus two FATs before the first byte
 * of data. A volume smaller than that formats, mounts, reports success -- and
 * then fails every write. That is exactly what a 16 KB RAM disk did on
 * RP2350 until this constant existed, and only a test that happened to write
 * a file noticed. Since 38.6 the FATs are sized from the volume (one sector
 * each at this size), so 64 sectors leaves 30 data clusters: small, but
 * coherent. */
#define FAT32_MIN_SECTORS 64

#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20

#define FAT32_EOF 0x0FFFFFF8

#pragma pack(push, 1)
typedef struct {
    uint8_t  jmp_boot[3];
    char     oem_name[8];
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clus;
    uint16_t reserved_sec_cnt;
    uint8_t  num_fats;
    uint16_t root_ent_cnt;
    uint16_t tot_sec16;
    uint8_t  media;
    uint16_t fat_sz16;
    uint16_t sec_per_trk;
    uint16_t num_heads;
    uint32_t hidd_sec;
    uint32_t tot_sec32;

    /* FAT32 Extended Fields */
    uint32_t fat_sz32;
    uint16_t ext_flags;
    uint16_t fs_ver;
    uint32_t root_clus;
    uint16_t fs_info;
    uint16_t bk_boot_sec;
    uint8_t  reserved[12];
    uint8_t  drv_num;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t vol_id;
    char     vol_lab[11];
    char     fil_sys_type[8];
} fat32_bpb_t;

typedef struct {
    char     name[11];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} fat32_dir_entry_t;
#pragma pack(pop)

/* free_count's "not counted yet", and FSInfo's own spelling of the same. */
#define FAT32_FREE_UNKNOWN 0xFFFFFFFFu

typedef struct {
    block_dev_t *dev;
    fat32_bpb_t bpb;
    uint32_t fat_start_sector;
    uint32_t data_start_sector;
    uint32_t root_dir_cluster;
    uint32_t bytes_per_cluster;

    /* 38.0, plan/open_issues.md "FAT32 file access is quadratic". Every
     * public call takes `lock`, so the cached state below is never seen
     * half-updated -- before it existed each call read the FAT into its own
     * stack buffer and there was nothing shared to tear. Re-entrant, because
     * the public calls use each other (write_at -> find_file). */
    ylock_t  lock;
    uint32_t total_clusters;   /* data clusters, numbered 2 .. total_clusters + 1 */
    uint32_t free_count;       /* FAT32_FREE_UNKNOWN until counted or read from FSInfo */
    uint32_t next_free;        /* where the allocator starts looking */
    uint32_t fsinfo_lba;       /* 0: the volume has no usable FSInfo sector */
    bool     fsinfo_valid;     /* the on-disk FSInfo holds a count we stand behind */
    bool     modified;         /* the FAT changed since mount; only then is FSInfo written */
    uint32_t gen;              /* bumped whenever a chain is freed: stale cursors see it */
    /* One FAT sector, write-through. Optional: NULL leaves the volume
     * uncached, which is right for /ram0 and /flash0, where a "read" is a
     * memcpy and 512 bytes of SRAM would buy nothing. fat_cache_lba 0 is
     * "empty" (sector 0 is the boot sector, never a FAT sector). */
    uint32_t *fat_cache;       /* 128 words */
    uint32_t fat_cache_lba;
} fat32_fs_t;

/* Where a handle last was in its file's cluster chain, so sequential
 * reads and writes continue from there instead of walking the chain from
 * the first cluster on every call. Zero-initialised is "nowhere". Valid
 * only while `first` and `gen` still match: a chain freed anywhere on the
 * volume (truncate, rm) bumps fs->gen, and every cursor falls back to a
 * walk from the start once rather than following a freed cluster. */
typedef struct {
    uint32_t first;            /* the file's first cluster; 0 = unset */
    uint32_t gen;
    uint32_t index;            /* cluster number within the file, from 0 */
    uint32_t cluster;
} fat32_cursor_t;

int fat32_init(fat32_fs_t *fs, block_dev_t *dev);

/* Gives a mounted volume a FAT-sector cache (512 bytes, 4-byte aligned,
 * owned by the caller for the volume's lifetime). Worth it on a real card,
 * where every uncached FAT lookup is a sector read over SPI or SDMMC. */
void fat32_set_fat_cache(fat32_fs_t *fs, uint32_t *buf128);

/* Writes the in-memory free count back to the FSInfo sector, if the FAT
 * changed since mount and the count is known. The first FAT change after a
 * sync marks FSInfo "unknown" on disk, so a board that loses power between
 * the two leaves a volume that recounts once, never one that lies. */
void fat32_sync(fat32_fs_t *fs);

/* Forgets the free count, counts the whole FAT, and writes the result to
 * FSInfo (`df -r`). For a card whose FSInfo another writer let go stale --
 * every card LugalOS wrote before 38.0, which never maintained it. Returns
 * the count it replaced. Slow on a large card: it reads the entire FAT. */
uint32_t fat32_recount(fat32_fs_t *fs);

/* Same, without the "no valid volume" line on failure (Y5a,
 * plan/phase31_concurrency_hierarchy.md).
 *
 * For the one caller where a blank volume is the *expected* state rather than
 * a problem: vfs_mount_ramdisk(), whose RAM disk starts empty on every boot
 * and is formatted immediately after. That narrated one expected event in
 * four boot lines -- probe failed, formatted, mounted, mounted on /ram0 --
 * of which only the last is news. On a real disk the message stays: a blank
 * or foreign-formatted SD card is exactly what a reader needs told. */
int fat32_init_quiet(fat32_fs_t *fs, block_dev_t *dev, bool quiet);
int fat32_format(block_dev_t *dev);

/* Same, without the "formatted cleanly" line -- same caller and same reason
 * as fat32_init_quiet(). An explicit (format ...) still says so. */
int fat32_format_quiet(block_dev_t *dev, bool quiet);
int fat32_find_file(fat32_fs_t *fs, const char *path, fat32_dir_entry_t *out_entry);
int fat32_read_file(fat32_fs_t *fs, fat32_dir_entry_t *entry, void *buf, uint32_t max_size);
int fat32_write_file(fat32_fs_t *fs, const char *path, const void *buf, uint32_t size);
int fat32_append_file(fat32_fs_t *fs, const char *path, const void *buf, uint32_t len);
int fat32_mkdir(fat32_fs_t *fs, const char *path);
int fat32_rmdir(fat32_fs_t *fs, const char *path);
int fat32_remove_file(fat32_fs_t *fs, const char *path);
void fat32_list_dir(fat32_fs_t *fs, const char *path);
/* Size and free space in 512-byte blocks (32 bits cover 2 TB; bytes would
 * not cover 4 GiB). The free count comes from FSInfo or the allocator's own
 * bookkeeping; only a volume whose FSInfo says "unknown" is scanned, once. */
int fat32_statfs(fat32_fs_t *fs, uint32_t *total_blocks, uint32_t *free_blocks);

/* Offset-addressed read/write, underlying fs/vfs_server.c's handle-based
 * vfs_pread()/vfs_pwrite() (see A1 in plan/phase5_distributed_design.md).
 * fat32_read_file() and fat32_append_file() are now thin wrappers over
 * these two. */
/* `cur` may be NULL (walk from the start, as before 38.0); a handle passes
 * its own so sequential access never re-walks the chain. */
int fat32_read_at(fat32_fs_t *fs, fat32_dir_entry_t *entry, void *buf, uint32_t count, uint64_t offset,
                  fat32_cursor_t *cur);
int fat32_write_at(fat32_fs_t *fs, const char *path, const void *buf, uint32_t count, uint64_t offset,
                   fat32_cursor_t *cur);

/* Frees an existing file's cluster chain and resets it to empty (size 0,
 * no first cluster) without deleting its directory entry -- the FAT32-level
 * primitive behind VFS_O_TRUNC. */
int fat32_truncate(fat32_fs_t *fs, const char *path);

/* Returns the `index`-th directory entry (0-based, in on-disk order,
 * including "." and ".." -- matching fat32_list_dir()'s existing behavior)
 * of the directory at `dir_cluster`. `name_out` receives a normalized
 * "NAME.EXT" (or "NAME") display string, not the raw space-padded 8.3
 * bytes. Returns -1 once `index` is past the last entry. */
int fat32_readdir(fat32_fs_t *fs, uint32_t dir_cluster, uint32_t index,
                   char *name_out, uint32_t name_max, fat32_dir_entry_t *out_entry);


#endif /* LUGALOS_FS_FAT32_H */

