#ifndef LUGALOS_FS_FAT32_H
#define LUGALOS_FS_FAT32_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/block.h"

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
/* The smallest volume fat32_format() can produce that is actually usable.
 *
 * Its layout spends 32 reserved sectors plus two 8-sector FATs, so 48 sectors
 * are gone before the first byte of data. A volume smaller than that formats,
 * mounts, reports success -- and then fails every write. That is exactly what
 * a 16 KB RAM disk did on RP2350 until this constant existed, and only a test
 * that happened to write a file noticed. 64 sectors leaves 16 data clusters:
 * small, but coherent. */
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

typedef struct {
    block_dev_t *dev;
    fat32_bpb_t bpb;
    uint32_t fat_start_sector;
    uint32_t data_start_sector;
    uint32_t root_dir_cluster;
    uint32_t bytes_per_cluster;
} fat32_fs_t;

int fat32_init(fat32_fs_t *fs, block_dev_t *dev);
int fat32_format(block_dev_t *dev);
int fat32_find_file(fat32_fs_t *fs, const char *path, fat32_dir_entry_t *out_entry);
int fat32_read_file(fat32_fs_t *fs, fat32_dir_entry_t *entry, void *buf, uint32_t max_size);
int fat32_write_file(fat32_fs_t *fs, const char *path, const void *buf, uint32_t size);
int fat32_append_file(fat32_fs_t *fs, const char *path, const void *buf, uint32_t len);
int fat32_mkdir(fat32_fs_t *fs, const char *path);
int fat32_rmdir(fat32_fs_t *fs, const char *path);
int fat32_remove_file(fat32_fs_t *fs, const char *path);
void fat32_list_dir(fat32_fs_t *fs, const char *path);
int fat32_statfs(fat32_fs_t *fs, uint32_t *total_bytes, uint32_t *free_bytes);

/* Offset-addressed read/write, underlying fs/vfs_server.c's handle-based
 * vfs_pread()/vfs_pwrite() (see A1 in plan/phase5_distributed_design.md).
 * fat32_read_file() and fat32_append_file() are now thin wrappers over
 * these two. */
int fat32_read_at(fat32_fs_t *fs, fat32_dir_entry_t *entry, void *buf, uint32_t count, uint64_t offset);
int fat32_write_at(fat32_fs_t *fs, const char *path, const void *buf, uint32_t count, uint64_t offset);

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

